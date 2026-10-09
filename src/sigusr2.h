#ifndef DWM_SIGUSR2_H
#define DWM_SIGUSR2_H

int sigusr2_setup(void);
int sigusr2_fd(void);
int sigusr2_dispatch(void (*reload)(void));
void sigusr2_cleanup(void);

#endif
