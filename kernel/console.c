//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100  // erase the last output character
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart, but don't use
// interrupts or sleep(). safe to be called from
// interrupts, e.g. by printf and to echo input
// characters.
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // input circular buffer
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index

  // Track a small subset of ANSI escape sequences (ESC [ A/B) so
  // the kernel doesn't echo them back and confuse the host terminal.
  int esc; // 0=none, 1=got ESC, 2=got ESC[
} cons;

//
// user write() system calls to the console go here.
// uses sleep() and UART interrupts.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[32]; // move batches from user space to uart.
  int i = 0;

  while(i < n){
    int nn = sizeof(buf);
    if(nn > n - i)
      nn = n - i;
    if(either_copyin(buf, user_src, src+i, nn) == -1)
      break;
    uartwrite(buf, nn);
    i += nn;
  }

  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dst indicates whether dst is a user
// or kernel address.
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    // In the original xv6 console, reads are line-buffered.
    // Here we return as soon as either:
    //  - we saw a newline, or
    //  - the input queue is empty (so programs can do char-at-a-time IO).
    if(c == '\n' || cons.r == cons.w)
      break;
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for each input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
void
consoleintr(int c)
{
  acquire(&cons.lock);

  if(c == C('P')){
    // Print process list.
    procdump();
    release(&cons.lock);
    return;
  }

  if(c == 0)
    goto done;

  if(c == '\r')
    c = '\n';

  // Maintain a tiny escape-sequence state to avoid echoing
  // arrow key sequences (ESC [ A/B). We must suppress echo for
  // *all* bytes in the sequence, including the final 'A'/'B'.
  int suppress_echo = 0;
  if(cons.esc == 0){
    if(c == 0x1b){
      suppress_echo = 1;
      cons.esc = 1;
    }
  } else if(cons.esc == 1){
    suppress_echo = 1;
    if(c == '[')
      cons.esc = 2;
    else
      cons.esc = 0;
  } else if(cons.esc == 2){
    suppress_echo = 1;
    cons.esc = 0;
  }

  // Store byte for consumption by consoleread().
  // Make it immediately readable (character-at-a-time input).
  if(cons.e-cons.r < INPUT_BUF_SIZE){
    cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;
    cons.w = cons.e;
    wakeup(&cons.r);
  }

  // Echo back to the user unless this byte is part of an escape sequence.
  // (ESC sequences are consumed by user programs like the shell.)
  if(suppress_echo == 0){
    if(c == '\t')
      goto done;
    if(c == 127 || c == C('H'))
      consputc(BACKSPACE);
    else
      consputc(c);
  }

done:
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
