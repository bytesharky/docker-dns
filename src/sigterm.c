#include "logging.h"  // for log_msg, LOG_DEBUG
#include "sigterm.h"
#include <signal.h>   // for signal, SIGINT, SIGTERM
#include <stdlib.h>   // for exit

volatile sig_atomic_t stop = 0;

void handle_sigterm(int sig) {
    log_msg(LOG_DEBUG, "Received signal %d", sig);
    stop = 1;
    exit(0);
}

void setup_signal_handlers(void) {
    signal(SIGINT, handle_sigterm);
    signal(SIGTERM, handle_sigterm);
}
