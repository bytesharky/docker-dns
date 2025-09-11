#ifndef SIGNAL_H
#define SIGNAL_H
#include <signal.h>   // for sig_atomic_t

extern volatile sig_atomic_t stop;

void setup_signal_handlers(void);
#endif
