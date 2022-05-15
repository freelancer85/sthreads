/* On Mac OS (aka OS X) the ucontext.h functions are deprecated and requires the
   following define.
*/
#define _XOPEN_SOURCE 700

/* On Mac OS when compiling with gcc (clang) the -Wno-deprecated-declarations
   flag must also be used to suppress compiler warnings.
*/

#include <signal.h>   /* SIGSTKSZ (default stack size), MINDIGSTKSZ (minimal
                         stack size) */
#include <stdio.h>    /* puts(), printf(), fprintf(), perror(), setvbuf(), _IOLBF,
                         stdout, stderr */
#include <stdlib.h>   /* exit(), EXIT_SUCCESS, EXIT_FAILURE, malloc(), free() */

#include <stdbool.h>  /* true, false */

#include <string.h>
#include <sys/time.h>  /* struct itimerval */

#include "sthreads.h"

/* Stack size for each context. */
#define STACK_SIZE SIGSTKSZ*100

/*******************************************************************************
                             Global data structures

                Add data structures to manage the threads here.
********************************************************************************/

/* ready queue */
static thread_t *rq[2] = {NULL, NULL};  //head is index 0, tail is at index 1
/* blocked queue for threads waiting on join */
static thread_t *bq[2] = {NULL, NULL};

/* current thread that is executing */
static thread_t *	t_executing = NULL;

/* last thread that called done() */
static thread_t *	t_exited = NULL;

/* our internal scheduler context */
static ucontext_t ctx_system;

/* context of caller ( in our case sthreads_test ) */
static ucontext_t ctx_user;

/* Our thread identificator */
static tid_t thread_id = 0;

/*******************************************************************************
                    Implementation of the Simple Threads API
********************************************************************************/

/* Setup alarm for preemption */
static int preemption_timer(void (*handler) (int), int ms) {

  struct itimerval timeslice;
  struct sigaction sa;

  memset(&sa, 0, sizeof(struct sigaction));
  memset(&timeslice, 0, sizeof(struct itimerval));

  sa.sa_handler = handler;
  sa.sa_flags = SA_RESTART | SA_SIGINFO;
  sigaction (SIGALRM, &sa, NULL);

  timeslice.it_value.tv_usec = ms*1000;
  if (setitimer (ITIMER_REAL, &timeslice, NULL) < 0) {
    perror("setitimer");
    return -1;
  }
  return 0;
}

/* Push thread at end of queue */
static void push(struct thread *q[2], thread_t *thr){

  thr->next = NULL;

  if(q[0] == NULL){
    q[0] = q[1] = thr;
  }else{
    q[0]->next = thr;
    q[1] = thr;
  }
}

/* Pop first thread from queue */
static thread_t * pop(struct thread *q[2]){
  thread_t * thr;

  if(q[0] == NULL){
    return NULL;
  }

  thr = q[0];
  thr->next = NULL;

  q[0] = q[0]->next;
  if(q[0]){
    q[1] = NULL;
  }

  return thr;
}

/* Unblock each thread waiting for thread that called done() */
static void unblock(const tid_t done_tid){
  thread_t * t = pop(bq);
  thread_t * last = bq[1];

  while(t){

    if(t->join_tid == done_tid){
      t->state = ready;
      fprintf(stderr, "unblock: unblocked %d, after %d exited\n", t->tid, done_tid);
      push(rq, t);
    }else{
      //fprintf(stderr, "unblock: requeue %d, on %d\n", t->tid, done_tid);
      push(bq, t);
    }

    if(t == last){
      break;
    }
    t = pop(bq);
  }
}

/* Context initialization */
static int context_init(ucontext_t *ctx, ucontext_t *next) {

  if (getcontext(ctx) < 0) {
    perror("getcontext");
    return -1;
  }

  ctx->uc_link           = next;
  ctx->uc_stack.ss_sp    = malloc(STACK_SIZE);
  if (ctx->uc_stack.ss_sp == NULL) {
    perror("calloc");
    return -1;
  }
  ctx->uc_stack.ss_size  = STACK_SIZE;
  ctx->uc_stack.ss_flags = 0;

  if (sigemptyset(&ctx->uc_sigmask) < 0){
     perror("sigemptyset");
     return -1;
  }
  return 0;
}

/* Initialize a thread */
static thread_t* thread_init(){

  thread_t * t = (thread_t *) malloc(sizeof(thread_t));
  if(t == NULL){
    perror("malloc");
    return NULL;
  }

  if(context_init(&t->ctx, &ctx_system) < 0){
    free(t);
    return NULL;
  }

  /* setup the thread */
  t->state = ready;
  t->tid = thread_id++;

  return t;
}

/* Deinitialize a thread */
static void thread_deinit(struct thread * t){

  fprintf(stderr,"deinit: Thread %d freed\n", t->tid);

  free(t->ctx.uc_stack.ss_sp);
  free(t);
}

/* Handler to preempt the executing thread */
static void preempt_thread(int sig){

  if(t_executing){

    fprintf(stderr,"preempt: Thread %d -> ready (preempted)\n", t_executing->tid);
    t_executing->state = ready;
    push(rq, t_executing);

    thread_t *t = t_executing;
    t_executing = NULL;

    /* change to scheduler context*/
    swapcontext(&t->ctx, &ctx_system);
  }else{
    /* scheduler context*/
    setcontext(&ctx_system);
  }
}

/* Schedule next thread */
static void system_scheduler(){

  /* deinit last exited thread */
  if(t_exited){
    thread_deinit(t_exited);
    t_exited = NULL;
  }

  /* if no threads are ready */
  if(rq[0] == NULL){
    fprintf(stderr,"system: No ready threads, returning to user context\n");
    setcontext(&ctx_user);
  }

  thread_t *t = pop(rq);
  t->state = running;

  if(t_executing){
    fprintf(stderr,"system: Thread %d is preempted\n", t_executing->tid);
    t_executing->state = ready;
    push(rq, t_executing);
  }

  fprintf(stderr,"system: Thread %d is running\n", t->tid);
  t_executing = t;

  /* set time to preempt thread after 15 ms */
  preemption_timer(preempt_thread, 15);

  if(setcontext(&t->ctx) < 0){
    perror("setcontext");
    return;
  }
}

int  init(){

  /* initialize system context */
  if(context_init(&ctx_system, NULL) < 0){
    return -1;
  }
  sigemptyset(&ctx_system.uc_sigmask);
  makecontext(&ctx_system, system_scheduler, 1);

  /* init the user context */
  getcontext(&ctx_user);

  return 1;
}

void deinit(){

  /* run all ready threads, before exit */
  while(rq[0]){

    /*disable preemption */
    preemption_timer(preempt_thread, 0);

    /* switch to manager */
    if(swapcontext(&ctx_user, &ctx_system) != 0){
      perror("swapcontext");
      exit(EXIT_FAILURE);
    }
  }

  fprintf(stderr, "deinit: total of %d threads created\n", thread_id);
}

tid_t spawn(void (*start)()){

  thread_t * t = thread_init();
  if(t == NULL){
    return -1;
  }
  makecontext(&t->ctx, start, 1);

  t->state = ready;
  push(rq, t);

  /*disable preemption */
  preemption_timer(preempt_thread, 0);

  /* swap to scheduler to schedule next thread */
  if(swapcontext(&ctx_user, &ctx_system) != 0){
    perror("swapcontext");
    exit(EXIT_FAILURE);
  }

  return t->tid;
}

void yield(){

  /*disable preemption */
  preemption_timer(preempt_thread, 0);

  /* swap to scheduler to schedule next thread */
  swapcontext(&t_executing->ctx, &ctx_system);
}

void done(){
  /* save id of terminating thread */
  const tid_t done_id = t_executing->tid;


  t_executing->state = terminated;
  fprintf(stderr,"done: Thread %d is terminated\n", done_id);

  /* save thread pointer, so we can clean its stack */
  t_exited = t_executing;
  t_executing = NULL;

  /* unblock all waiting to join this thread */
  unblock(done_id);

  /*disable preemption */
  preemption_timer(preempt_thread, 0);

  /* swap to scheduler to schedule next thread */
  setcontext(&ctx_system);
}

tid_t join(const int wait_tid) {

  /* save thread that called join */
  thread_t * t_blocked = t_executing;
  t_blocked->join_tid = wait_tid;

  /* suspended thread goes to waiting queue */
  t_blocked->state = waiting;
  fprintf(stderr,"join: Thread %d is waiting for %d\n", t_blocked->tid, wait_tid);

  push(bq, t_blocked);
  t_executing = NULL;

  /*disable preemption */
  preemption_timer(preempt_thread, 0);

  /* swap to scheduler to schedule next thread */
  if(swapcontext(&t_blocked->ctx, &ctx_system) != 0){
    perror("swapcontext");
  }

  t_blocked->state = running;
  fprintf(stderr,"join: Thread %d is running, joined with %d\n", t_blocked->tid, t_blocked->join_tid);

  return t_blocked->join_tid;
}

int lock_init(mutex_t *l){

  if(l == NULL){
    fprintf(stderr, "lock_deinit: null lock\n");
    return -1;
  }

  if(sem_init(l, 0, 1) == -1){
    perror("lock_init");
    return -1;
  }

  return 0;
}

int lock_deinit(mutex_t *l){
  if(l == NULL){
    fprintf(stderr, "lock_deinit: null lock\n");
    return -1;
  }

  if(sem_destroy(l) == -1){
    perror("lock_deinit");
    return -1;
  }

  return 0;
}

int lock(mutex_t *l){

  if(l == NULL){
    fprintf(stderr, "unlock: null lock\n");
    return -1;
  }

  if(sem_wait(l) == -1){
    perror("lock");
    return -1;
  }
  return 0;
}

int unlock(mutex_t *l){

  if(l == NULL){
    fprintf(stderr, "unlock: null lock\n");
    return -1;
  }

  if(sem_post(l) == -1){
    perror("unlock");
    return -1;
  }

  return 0;
}
