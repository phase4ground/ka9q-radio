// $Id: demod.c,v 1.18 2017/06/04 10:52:09 karn Exp karn $
// Common demod thread for all modes
// Takes commands from UDP packets on a socket
#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <complex.h>
#undef I

#include "radio.h"


float dc_alpha = 0.00001; // high pass filter coefficient for offset and I/Q imbalance estimates
float power_alpha = 0.00001; // high pass filter coefficient for power estimates

const float SCALE = 1./32768.;

// Preprocessing of samples performed for all demodulators
// Remove DC biases, equalize I/Q power, correct phase imbalance
// Update power measurement
void proc_samples(struct demod *demod,const short *sp,const int cnt){
  // Channel gain balance coefficients
  float gain_i=1,gain_q=1,sinphi = 0,secphi=1,tanphi = 0;
  if(demod->power_i != 0 && demod->power_q != 0){
    gain_q = sqrtf((demod->power_i + demod->power_q)/(2*demod->power_q));
    gain_i = sqrtf((demod->power_i + demod->power_q)/(2*demod->power_i));
    sinphi = demod->dotprod / (demod->power_i + demod->power_q);
    if(fabs(sinphi) >= 0.9999){
      // Make sure it can't exceed [-1,+1]
      sinphi = copysignf(0.9999,sinphi);
    }
    secphi = 1/sqrtf(1 - sinphi * sinphi);
    tanphi = sinphi * secphi;
  }

  int i;
  complex float buffer[cnt];
  for(i=0;i<cnt;i++){
    float samp_i,samp_q;
    // Remove and update DC offsets
    samp_i = sp[2*i] * SCALE - demod->DC_i;    samp_q = sp[2*i+1] * SCALE - demod->DC_q;
    demod->DC_i += dc_alpha * samp_i;  demod->DC_q += dc_alpha * samp_q;
    // Update channel power estimates
    demod->power_i += power_alpha * (samp_i * samp_i - demod->power_i);
    demod->power_q += power_alpha * (samp_q * samp_q - demod->power_q);    
    // Balance gains, keeping constant total energy
    samp_i *= gain_i;                  samp_q *= gain_q;
    // Correct phase
    samp_q = samp_q * secphi - tanphi*samp_i;
    assert(!isnan(samp_q) && !isnan(samp_i));
    // Update residual phase error estimate
    demod->dotprod += power_alpha * ((samp_i * samp_q) - demod->dotprod); 
    // Final corrected sample
    buffer[i] = CMPLXF(samp_i,samp_q);
  }
  // Pass to demodulator thread (ssb/fm/iq etc)
  write(Demod_sock,buffer,sizeof(buffer));
}
