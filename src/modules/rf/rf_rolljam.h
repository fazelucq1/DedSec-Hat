#ifndef __RF_ROLLJAM_H__
#define __RF_ROLLJAM_H__

// Experimental, single-radio approximation of the RollJam attack (Kamkar,
// DEFCON 2015) for the CC1101. See rf_rolljam.cpp for how and why it differs
// from the textbook two-radio attack. Intended for testing your own
// equipment's rolling-code resilience - not a guaranteed bypass.
void rf_rolljam();

#endif
