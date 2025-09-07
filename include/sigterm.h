#ifndef SIGNAL_H
#define SIGNAL_H
#include <signal.h>

extern volatile sig_atomic_t stop;

void setup_signal_handlers(void);
#endif
