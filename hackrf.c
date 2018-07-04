// $Id: hackrf.c,v 1.3 2018/07/02 17:12:06 karn Exp karn $
// Read from HackRF
// Multicast raw 8-bit I/Q samples
// Accept control commands from UDP socket
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <locale.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <libhackrf/hackrf.h>


#include "sdr.h"
#include "radio.h"
#include "misc.h"
#include "multicast.h"
#include "decimate.h"


// decibel limits for power
float Upper_threshold = 30;
float Lower_threshold = 10;
int Agc_holdoff = 10;
#define BUFFERSIZE (1<<19) // Upcalls seem to be 256KB; don't make too big or we may blow out of the cache
int ADC_samprate = 3072000;
int Out_samprate = 192000;
int Decimate = 128;
int Log_decimate = 7;
int Blocksize = 350;
int Device = 0;
float const DC_alpha = 0.01;    // high pass filter coefficient for DC offset estimates, per sample
float const Power_alpha = 0.01; // high pass filter coefficient for power and I/Q imbalance estimates, per sample


int Verbose;
int Device;       // Which of several to use
int Offset=1;     // Default to offset high by +Fs/4 downconvert in software to avoid DC

struct sdrstate {
  // Stuff for sending commands
  struct status status;     // Frequency and gain settings, grouped for transmission in RTP packet

  // Analog gain settings
  int agc_holdoff_count;

  float power;              // Running estimate of unfiltered signal power
  long long clips;          // peak range samples, proxy for clipping

  hackrf_device *device;
  complex float DC;
  float sinphi;          // smoothed estimate of I/Q phase error
  float imbalance;       // Ratio of I power to Q power
};

struct sdrstate HackCD;
char *Locale;


pthread_t Display_thread;
pthread_t Process_thread;


int Rtp_sock; // Socket handle for sending real time stream *and* receiving commands
int Ctl_sock;
extern int Mcast_ttl;
long Ssrc;
int Seq = 0;
int Timestamp = 0;

complex float Sampbuffer[BUFFERSIZE];
int Samp_wp;
int Samp_rp;


pthread_mutex_t Buf_mutex;
pthread_cond_t Buf_cond;

void *display(void *arg);
double  rffc5071_freq(uint16_t lo);
uint32_t max2837_freq(uint32_t freq);


// Gain and phase corrections. These will be updated every block
float gain_q = 1;
float gain_i = 1;
float secphi = 1;
float tanphi = 0;

// Callback called with incoming receiver data from A/D
int rx_callback(hackrf_transfer *transfer){
  int remain = transfer->valid_length;
  unsigned char *dp = transfer->buffer;

  complex float samp_sum = 0;
  complex float samp_sq_sum = 0;
  float dotprod = 0;                           // sum of I*Q, for phase balance
  float blocks_p_samp = 1./remain; // avoid divisions later

  while(remain > 0){
    complex float samp;
    int isamp_i = (char)*dp++;
    int isamp_q = (char)*dp++;
    remain -= 2;

    if(abs(isamp_q) >= 127 || abs(isamp_i) >= 127)
      HackCD.clips++;

    samp = CMPLXF(isamp_i,isamp_q);

    // Update DC totals - can be done as integers
    // Is this necessary when Offset is active?
    samp_sum += samp;

    // Convert to float, remove DC offset (which can be fractional)
    samp -= HackCD.DC;
    
    // Must correct gain and phase before frequency shift
    // accumulate I and Q energies before gain correction
    samp_sq_sum += CMPLXF(crealf(samp) * crealf(samp), cimagf(samp) * cimagf(samp));
    
    // Balance gains, keeping constant total energy
    __real__ samp *= gain_i;
    __imag__ samp *= gain_q;
    
    // Accumulate phase error
    dotprod += crealf(samp) * cimagf(samp);

    // Correct phase
    __imag__ samp = secphi * cimagf(samp) - tanphi * crealf(samp);
    

    Sampbuffer[Samp_wp] = samp;
    Samp_wp = (Samp_wp + 1) & (BUFFERSIZE-1);
  }
  pthread_cond_signal(&Buf_cond); // Wake him up only after we're done
  // Update every block
  // estimates of DC offset, signal powers and phase error
  HackCD.DC += DC_alpha * (blocks_p_samp * samp_sum - HackCD.DC);
  float block_energy = crealf(samp_sq_sum) + cimagf(samp_sq_sum);
  if(block_energy > 0){ // Avoid divisions by 0, etc
    HackCD.power += Power_alpha * (0.5*blocks_p_samp * block_energy - HackCD.power); // Average A/D output power per channel
    HackCD.imbalance += Power_alpha *
      (crealf(samp_sq_sum) / cimagf(samp_sq_sum) - HackCD.imbalance);
    float dpn = 2 * dotprod / block_energy;
    HackCD.sinphi += Power_alpha * (dpn - HackCD.sinphi);
    gain_q = sqrtf(0.5 * (1 + HackCD.imbalance));
    gain_i = sqrtf(0.5 * (1 + 1./HackCD.imbalance));
    secphi = 1/sqrtf(1 - HackCD.sinphi * HackCD.sinphi); // sec(phi) = 1/cos(phi)
    tanphi = HackCD.sinphi * secphi;                     // tan(phi) = sin(phi) * sec(phi) = sin(phi)/cos(phi)
  }
  return 0;
}

void *process(void *arg){

  pthread_setname("hackrf-proc");

  unsigned char buffer[200+2*Blocksize*sizeof(short)];
  struct rtp_header rtp;
  memset(&rtp,0,sizeof(rtp));
  rtp.version = RTP_VERS;
  rtp.type = IQ_PT;
  rtp.ssrc = Ssrc;
  int rotate_phase = 0;

  // Filter states

  struct hb15_state hb_state_real[Log_decimate];
  struct hb15_state hb_state_imag[Log_decimate];
  memset(hb_state_real,0,sizeof(hb_state_real));
  memset(hb_state_imag,0,sizeof(hb_state_imag));

  // Initialize coefficients here!!!
  // As experiment, use Goodman/Carey "F8" 15-tap filter
  // Note word order in array -- [3] is closest to the center, [0] is on the tails
  for(int i=0; i<Log_decimate; i++){ // For each stage (h(0) is always unity, other h(n) are zero for even n)
    hb_state_real[i].coeffs[3] = 490./802;
    hb_state_imag[i].coeffs[3] = 490./802;
    hb_state_real[i].coeffs[2] = -116./802;
    hb_state_imag[i].coeffs[2] = -116./802;
    hb_state_real[i].coeffs[1] = 33./802; 
    hb_state_imag[i].coeffs[1] = 33./802;    
    hb_state_real[i].coeffs[0] = -6./802; 
    hb_state_imag[i].coeffs[0] = -6./802;    
  }
  float time_p_packet = Blocksize / Out_samprate;
  while(1){

    rtp.timestamp = Timestamp;
    rtp.seq = Seq++;

    unsigned char *dp = buffer;
    dp = hton_rtp(dp,&rtp);
    dp = hton_status(dp,&HackCD.status);

    // Wait for enough to be available
    pthread_mutex_lock(&Buf_mutex);
    while(1){
      int avail = (Samp_wp - Samp_rp) & (BUFFERSIZE-1);
      if(avail >= Blocksize*Decimate)
	break;
      pthread_cond_wait(&Buf_cond,&Buf_mutex);
    }
    pthread_mutex_unlock(&Buf_mutex);
    
    float workblock_real[Decimate*Blocksize];    // Hold input to first decimator, half used on each filter call
    float workblock_imag[Decimate*Blocksize];

    // Load first stage with corrected samples
    int loop_limit = Decimate * Blocksize;
    for(int i=0; i<loop_limit; i++){
      complex float samp = Sampbuffer[Samp_rp++];
      float samp_i = crealf(samp);
      float samp_q = cimagf(samp);
      Samp_rp &= (BUFFERSIZE-1); // Assume even buffer size

      // Increase frequency by Fs/4 to compensate for tuner being high by Fs/4
      switch(rotate_phase){
      default:
      case 0:
	workblock_real[i] = samp_i;
	workblock_imag[i] = samp_q;
      case 1:
	workblock_real[i] = -samp_q;
	workblock_imag[i] = samp_i;
	break;
      case 2:
	workblock_real[i] = -samp_i;
	workblock_imag[i] = -samp_q;
	break;
      case 3:
	workblock_real[i] = samp_q;
	workblock_imag[i] = -samp_i;
	break;
      }
      rotate_phase += Offset;
      rotate_phase &= 3; // Modulo 4
    }
    // Real channel decimation
    for(int j=Log_decimate-1;j>=0;j--)
      hb15_block(&hb_state_real[j],workblock_real,workblock_real,(1<<j)*Blocksize);

    signed short *up = (signed short *)dp;
    loop_limit = Blocksize;
    for(int j=0;j<loop_limit;j++){
      *up++ = (short)round(workblock_real[j]);
      up++;
    }

    // Imaginary channel decimation
    for(int j=Log_decimate-1;j>=0;j--)
      hb15_block(&hb_state_imag[j],workblock_imag,workblock_imag,(1<<j)*Blocksize);      

    // Interleave imaginary samples following real
    up = (signed short *)dp;
    loop_limit = Blocksize;
    for(int j=0;j<loop_limit;j++){
      up++;
      *up++ = (short)round(workblock_imag[j]);
    }
    dp = (unsigned char *)up;
    if(send(Rtp_sock,buffer,dp - buffer,0) == -1){
      perror("send");
      // If we're sending to a unicast address without a listener, we'll get ECONNREFUSED
      // Sleep 1 sec to slow down the rate of these messages
      usleep(1000000);
    }
    Timestamp += Blocksize; // samples
  
    // Simply increment by number of samples
    // But what if we lose some? Then the clock will always be off
    HackCD.status.timestamp += 1.e9 * time_p_packet;

  }
}


int main(int argc,char *argv[]){
  // if we have root, up our priority and drop privileges
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 10);

  // Quickly drop root if we have it
  // The sooner we do this, the fewer options there are for abuse
  if(seteuid(getuid()) != 0)
    perror("seteuid");

  char *dest = "239.1.6.1"; // Default for testing

  Locale = getenv("LANG");
  if(Locale == NULL || strlen(Locale) == 0)
    Locale = "en_US.UTF-8";

  int c;
  while((c = getopt(argc,argv,"D:d:vp:l:b:R:T:o:r:")) != EOF){
    switch(c){
    case 'o':
      Offset = strtol(optarg,NULL,0);
      break;
    case 'r':
      Out_samprate = strtol(optarg,NULL,0);
      break;
    case 'R':
      dest = optarg;
      break;
    case 'd':
      Decimate = strtol(optarg,NULL,0);
      break;
    case 'D':
      Device = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose++;
      break;
    case 'l':
      Locale = optarg;
      break;
    case 'b':
      Blocksize = strtol(optarg,NULL,0);
      break;
    case 'T':
      Mcast_ttl = strtol(optarg,NULL,0);
      break;
    }
  }
  ADC_samprate = Decimate * Out_samprate;
  Log_decimate = (int)round(log2(Decimate));
  if(1<<Log_decimate != Decimate){
    fprintf(stderr,"Decimation ratios must currently be a power of 2\n");
    exit(1);
  }

  setlocale(LC_ALL,Locale);
  
  // Set up RTP output socket
  Rtp_sock = setup_mcast(dest,1);
  if(Rtp_sock == -1){
    perror("Can't create multicast socket");
    exit(1);
  }
    
  // Set up control socket
  Ctl_sock = socket(AF_INET,SOCK_DGRAM,0);

  // bind control socket to next sequential port after our multicast source port
  struct sockaddr_in ctl_sockaddr;
  socklen_t siz = sizeof(ctl_sockaddr);
  if(getsockname(Rtp_sock,(struct sockaddr *)&ctl_sockaddr,&siz) == -1){
    perror("getsockname on ctl port");
    exit(1);
  }
  struct sockaddr_in locsock;
  locsock.sin_family = AF_INET;
  locsock.sin_port = htons(ntohs(ctl_sockaddr.sin_port)+1);
  locsock.sin_addr.s_addr = INADDR_ANY;
  bind(Ctl_sock,(struct sockaddr *)&locsock,sizeof(locsock));

  int ret;
  if((ret = hackrf_init()) != HACKRF_SUCCESS){
    fprintf(stderr,"hackrf_init() failed: %s\n",hackrf_error_name(ret));
    exit(1);
  }
  // Enumerate devices
  hackrf_device_list_t *dlist = hackrf_device_list();

  if((ret = hackrf_device_list_open(dlist,Device,&HackCD.device)) != HACKRF_SUCCESS){
    fprintf(stderr,"hackrf_open(%d) failed: %s\n",Device,hackrf_error_name(ret));
    exit(1);
  }
  hackrf_device_list_free(dlist); dlist = NULL;

  ret = hackrf_set_sample_rate(HackCD.device,(double)ADC_samprate);
  assert(ret == HACKRF_SUCCESS);
  HackCD.status.samprate = Out_samprate;
  uint32_t bw = hackrf_compute_baseband_filter_bw_round_down_lt(ADC_samprate);
  ret = hackrf_set_baseband_filter_bandwidth(HackCD.device,bw);
  assert(ret == HACKRF_SUCCESS);

  // NOTE: what we call mixer gain, they call lna gain
  // What we call lna gain, they call antenna enable
  HackCD.status.lna_gain = 1;
  HackCD.status.mixer_gain = 24;
  HackCD.status.if_gain = 20;

  ret = hackrf_set_antenna_enable(HackCD.device,HackCD.status.lna_gain);
  assert(ret == HACKRF_SUCCESS);
  ret = hackrf_set_lna_gain(HackCD.device,HackCD.status.mixer_gain);
  assert(ret == HACKRF_SUCCESS);
  ret = hackrf_set_vga_gain(HackCD.device,HackCD.status.if_gain);
  assert(ret == HACKRF_SUCCESS);

  uint64_t intfreq = HackCD.status.frequency = 146000000;
  
  intfreq += Offset * ADC_samprate / 4; // Offset tune high by +Fs/4

  ret = hackrf_set_freq(HackCD.device,intfreq);
  assert(ret == HACKRF_SUCCESS);

  pthread_mutex_init(&Buf_mutex,NULL);
  pthread_cond_init(&Buf_cond,NULL);


  time_t tt;
  time(&tt);
  struct timeval tp;
  gettimeofday(&tp,NULL);
  // Timestamp is in nanoseconds for futureproofing, but time of day is only available in microsec
  HackCD.status.timestamp = ((tp.tv_sec - UNIX_EPOCH + GPS_UTC_OFFSET) * 1000000LL + tp.tv_usec) * 1000LL;

  Ssrc = tt & 0xffffffff; // low 32 bits of clock time
  if(Verbose){
    fprintf(stderr,"hackrf device %d; blocksize %d; RTP SSRC %lx\n",Device,Blocksize,Ssrc);
    fprintf(stderr,"A/D sample rate %'d Hz; decimation ratio %d; output sample rate %'d Hz; Offset %'+d\n",
	    ADC_samprate,Decimate,Out_samprate,Offset * ADC_samprate/4);
  }
  pthread_create(&Process_thread,NULL,process,NULL);

  ret = hackrf_start_rx(HackCD.device,rx_callback,&HackCD);
  assert(ret == HACKRF_SUCCESS);

  if(Verbose > 1){
    signal(SIGPIPE,SIG_IGN);
    signal(SIGINT,closedown);
    signal(SIGKILL,closedown);
    signal(SIGQUIT,closedown);
    signal(SIGTERM,closedown);        

    pthread_create(&Display_thread,NULL,display,NULL);
  }

  // Process commands to change hackrf state
  // We listen on the same IP address and port we use as a multicasting source
  pthread_setname("hackrf-cmd");

  while(1){

    fd_set fdset;
    socklen_t addrlen;
    int ret;
    struct timeval timeout;
    struct status requested_status;
    
    // Read with a timeout - necessary?
    FD_ZERO(&fdset);
    FD_SET(Ctl_sock,&fdset);
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    ret = select(Ctl_sock+1,&fdset,NULL,NULL,&timeout);
    if(ret == -1){
      perror("select");
      usleep(50000); // don't loop tightly
      continue;
    }
    if(ret == 0)
      continue;

    // A command arrived, read it
    // Should probably log these
    struct sockaddr_in6 command_address;
    addrlen = sizeof(command_address);
    if((ret = recvfrom(Ctl_sock,&requested_status,sizeof(requested_status),0,(struct sockaddr *)&command_address,&addrlen)) <= 0){
      if(ret < 0)
	perror("recv");
      usleep(50000); // don't loop tightly
      continue;
    }
    
    if(ret < sizeof(requested_status))
      continue; // Too short; ignore
    
    uint64_t intfreq = HackCD.status.frequency = requested_status.frequency;
    intfreq += Offset * ADC_samprate/4; // Offset tune by +Fs/4

    ret = hackrf_set_freq(HackCD.device,intfreq);
    assert(ret == HACKRF_SUCCESS);
  }
  // Can't really get here
  close(Rtp_sock);
  hackrf_close(HackCD.device);
  hackrf_exit();
  exit(0);
}



// Status display thread
void *display(void *arg){
  pthread_setname("hackrf-disp");

  double  rffc5071_freq(uint16_t lo);
  uint32_t max2837_freq(uint32_t freq);

  fprintf(stderr,"               |-------GAINS-----| Level DC offsets  ---Errors--           clips\n");
  fprintf(stderr,"Frequency      LNA  mixer baseband  A/D     I     Q  phase  gain\n");
  fprintf(stderr,"Hz                     dB  dB        dB                deg    dB\n");   

  while(1){
    float powerdB = 10*log10f(HackCD.power);

    fprintf(stderr,"%'-15.0lf%3d%7d%4d%'10.1f%6.1f%6.1f%7.2f%6.2f%'16lld    \r",
	    HackCD.status.frequency,
	    HackCD.status.lna_gain,	    
	    HackCD.status.mixer_gain,
	    HackCD.status.if_gain,
	    powerdB,
	    crealf(HackCD.DC),
	    cimagf(HackCD.DC),
	    (180/M_PI) * asin(HackCD.sinphi),
	    10*log10(HackCD.imbalance),
	    HackCD.clips);
    


    usleep(100000); // 10 Hz
    int ret __attribute__((unused)) = HACKRF_SUCCESS; // Won't be used when asserts are disabled
    if(HackCD.agc_holdoff_count){
      HackCD.agc_holdoff_count--;
    } else if(isnan(powerdB) || powerdB < Lower_threshold){
      // Turn on external antenna first, start counter
      if(HackCD.status.lna_gain < 1){
	HackCD.status.lna_gain++;
	ret = hackrf_set_antenna_enable(HackCD.device,HackCD.status.lna_gain);
      } else if(HackCD.status.mixer_gain < 40){
	HackCD.status.mixer_gain += 8;
	ret = hackrf_set_lna_gain(HackCD.device,HackCD.status.mixer_gain);
      } else if(HackCD.status.if_gain < 62){
	HackCD.status.if_gain += 2;
	ret = hackrf_set_vga_gain(HackCD.device,HackCD.status.if_gain);
      }
      HackCD.agc_holdoff_count = Agc_holdoff;
      assert(ret == HACKRF_SUCCESS);
    } else if(powerdB > Upper_threshold){
      // Reduce gain (IF first), start counter
      if(HackCD.status.if_gain > 0){
	HackCD.status.if_gain -= 2;
	ret = hackrf_set_vga_gain(HackCD.device,HackCD.status.if_gain);
      } else if(HackCD.status.mixer_gain > 0){
	HackCD.status.mixer_gain -= 8;
	ret = hackrf_set_lna_gain(HackCD.device,HackCD.status.mixer_gain);
      } else if(HackCD.status.lna_gain > 0){
	HackCD.status.lna_gain--;
	ret = hackrf_set_antenna_enable(HackCD.device,HackCD.status.lna_gain);
      }
      assert(ret == HACKRF_SUCCESS);
      HackCD.agc_holdoff_count = Agc_holdoff;
    }
  }
  return NULL;
}



void closedown(int a){
  if(Verbose)
    fprintf(stderr,"\nhackrf: caught signal %d: %s\n",a,strsignal(a));

  exit(1);
}

// extracted from hackRF firmware/common/rffc5071.c
// Used to set RFFC5071 upconverter to multiples of 1 MHz
// for future use in determining exact tuning frequency

#define LO_MAX 5400.0
#define REF_FREQ 50.0
#define FREQ_ONE_MHZ (1000.0*1000.0)

double  rffc5071_freq(uint16_t lo) {
	uint8_t lodiv;
	uint16_t fvco;
	uint8_t fbkdiv;
	
	/* Calculate n_lo */
	uint8_t n_lo = 0;
	uint16_t x = LO_MAX / lo;
	while ((x > 1) && (n_lo < 5)) {
		n_lo++;
		x >>= 1;
	}

	lodiv = 1 << n_lo;
	fvco = lodiv * lo;

	if (fvco > 3200) {
		fbkdiv = 4;
	} else {
		fbkdiv = 2;
	}

	uint64_t tmp_n = ((uint64_t)fvco << 29ULL) / (fbkdiv*REF_FREQ) ;

	return (REF_FREQ * (tmp_n >> 5ULL) * fbkdiv * FREQ_ONE_MHZ)
			/ (lodiv * (1 << 24ULL));
}
uint32_t max2837_freq(uint32_t freq){
	uint32_t div_frac;
	//	uint32_t div_int;
	uint32_t div_rem;
	uint32_t div_cmp;
	int i;

	/* ASSUME 40MHz PLL. Ratio = F*(4/3)/40,000,000 = F/30,000,000 */
	//	div_int = freq / 30000000;
       	div_rem = freq % 30000000;
	div_frac = 0;
	div_cmp = 30000000;
	for( i = 0; i < 20; i++) {
		div_frac <<= 1;
		div_cmp >>= 1;
		if (div_rem > div_cmp) {
			div_frac |= 0x1;
			div_rem -= div_cmp;
		}
	}
	return div_rem;

}
#if 0

#define FREQ_ONE_MHZ     (1000*1000)

#define MIN_LP_FREQ_MHZ (0)
#define MAX_LP_FREQ_MHZ (2150)

#define MIN_BYPASS_FREQ_MHZ (2150)
#define MAX_BYPASS_FREQ_MHZ (2750)

#define MIN_HP_FREQ_MHZ (2750)
#define MID1_HP_FREQ_MHZ (3600)
#define MID2_HP_FREQ_MHZ (5100)
#define MAX_HP_FREQ_MHZ (7250)

#define MIN_LO_FREQ_HZ (84375000)
#define MAX_LO_FREQ_HZ (5400000000ULL)

static uint32_t max2837_freq_nominal_hz=2560000000;

uint64_t freq_cache = 100000000;
/*
 * Set freq/tuning between 0MHz to 7250 MHz (less than 16bits really used)
 * hz between 0 to 999999 Hz (not checked)
 * return false on error or true if success.
 */
bool set_freq(const uint64_t freq)
{
	bool success;
	uint32_t RFFC5071_freq_mhz;
	uint32_t MAX2837_freq_hz;
	uint64_t real_RFFC5071_freq_hz;

	const uint32_t freq_mhz = freq / 1000000;
	const uint32_t freq_hz = freq % 1000000;

	success = true;

	const max2837_mode_t prior_max2837_mode = max2837_mode(&max2837);
	max2837_set_mode(&max2837, MAX2837_MODE_STANDBY);
	if(freq_mhz < MAX_LP_FREQ_MHZ)
	{
		rf_path_set_filter(&rf_path, RF_PATH_FILTER_LOW_PASS);
		/* IF is graduated from 2650 MHz to 2343 MHz */
		max2837_freq_nominal_hz = 2650000000 - (freq / 7);
		RFFC5071_freq_mhz = (max2837_freq_nominal_hz / FREQ_ONE_MHZ) + freq_mhz;
		/* Set Freq and read real freq */
		real_RFFC5071_freq_hz = rffc5071_set_frequency(&rffc5072, RFFC5071_freq_mhz);
		max2837_set_frequency(&max2837, real_RFFC5071_freq_hz - freq);
		sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 1);
	}else if( (freq_mhz >= MIN_BYPASS_FREQ_MHZ) && (freq_mhz < MAX_BYPASS_FREQ_MHZ) )
	{
		rf_path_set_filter(&rf_path, RF_PATH_FILTER_BYPASS);
		MAX2837_freq_hz = (freq_mhz * FREQ_ONE_MHZ) + freq_hz;
		/* RFFC5071_freq_mhz <= not used in Bypass mode */
		max2837_set_frequency(&max2837, MAX2837_freq_hz);
		sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 0);
	}else if(  (freq_mhz >= MIN_HP_FREQ_MHZ) && (freq_mhz <= MAX_HP_FREQ_MHZ) )
	{
		if (freq_mhz < MID1_HP_FREQ_MHZ) {
			/* IF is graduated from 2150 MHz to 2750 MHz */
			max2837_freq_nominal_hz = 2150000000 + (((freq - 2750000000) * 60) / 85);
		} else if (freq_mhz < MID2_HP_FREQ_MHZ) {
			/* IF is graduated from 2350 MHz to 2650 MHz */
			max2837_freq_nominal_hz = 2350000000 + ((freq - 3600000000) / 5);
		} else {
			/* IF is graduated from 2500 MHz to 2738 MHz */
			max2837_freq_nominal_hz = 2500000000 + ((freq - 5100000000) / 9);
		}
		rf_path_set_filter(&rf_path, RF_PATH_FILTER_HIGH_PASS);
		RFFC5071_freq_mhz = freq_mhz - (max2837_freq_nominal_hz / FREQ_ONE_MHZ);
		/* Set Freq and read real freq */
		real_RFFC5071_freq_hz = rffc5071_set_frequency(&rffc5072, RFFC5071_freq_mhz);
		max2837_set_frequency(&max2837, freq - real_RFFC5071_freq_hz);
		sgpio_cpld_stream_rx_set_q_invert(&sgpio_config, 0);
	}else
	{
		/* Error freq_mhz too high */
		success = false;
	}
	max2837_set_mode(&max2837, prior_max2837_mode);
	if( success ) {
		freq_cache = freq;
	}
	return success;
}

#endif