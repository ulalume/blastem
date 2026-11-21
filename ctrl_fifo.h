#ifndef CTRL_FIFO_H_
#define CTRL_FIFO_H_

void ctrl_fifo_poll(void);

// Run the commands in $BLASTEM_CTRL_INIT (one per line) once, before the machine's first
// instruction. No-op when the variable is unset. See ctrl_fifo.c.
void ctrl_fifo_run_init(void);

#endif //CTRL_FIFO_H_
