#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

static int nthread = 1;
static int round = 0;

struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  int nthread;      // Number of threads that have reached this round of the barrier
  int round;     // Barrier round
} bstate;

static void
barrier_init(void)
{
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

static void 
barrier()
{
  // YOUR CODE HERE
  //
  // Block until all threads have called barrier() and
  // then increment bstate.round.
  //
  // 考虑到：线程1进入barrier之后，bstate中的nthread + 1， 但此时没有达到全局的nthread
  // 线程1 刚要进入睡眠，此时线程2 进入了barrier， 达到了全局的nthread，调用pthread_cond_wait来唤醒所有进程
  // 而此时线程1 才进入睡眠，导致线程1 没被唤醒
  // 因此，在bstate中的nthread + 1 到调用 pthread_cond_wait来进入睡眠，这个过程应该上锁
  // pthread_cond_wait 会释放当前锁，避免其他线程拿不到锁
  pthread_mutex_lock(&bstate.barrier_mutex);
  if(++bstate.nthread < nthread)
  {
    // 没有达到全局的nthread，睡眠
    // 函数原型解释：阻塞当前进程，并释放互斥锁,等待条件变量被触发
    pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
  }
  else
  {
    // 达到全局的nthread，重置，释放
    bstate.nthread = 0;
    bstate.round++;
    // 函数原型解释：唤醒所有等待条件变量的线程
    pthread_cond_broadcast(&bstate.barrier_cond);
  }
  pthread_mutex_unlock(&bstate.barrier_mutex);
}

static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;
    assert (i == t);
    barrier();
    usleep(random() % 100);
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}
