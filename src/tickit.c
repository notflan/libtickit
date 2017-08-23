#include "tickit.h"

#include <signal.h>
#include <sys/time.h>

/* INTERNAL */
TickitWindow* tickit_window_new_root2(Tickit *t, TickitTerm *term);

typedef struct Deferral Deferral;

struct Deferral {
  Deferral *next;

  int id;
  struct timeval at;  /* only for timers */
  TickitCallbackFn *fn;
  void *user;
};

struct Tickit {
  int refcount;

  volatile int still_running;

  TickitTerm   *term;
  TickitWindow *rootwin;

  Deferral *timers;
  int next_timer_id;

  Deferral *laters;
};

Tickit *tickit_new_stdio(void)
{
  Tickit *t = malloc(sizeof(Tickit));
  if(!t)
    return NULL;

  t->refcount = 1;

  t->term = NULL;
  t->rootwin = NULL;

  t->timers = NULL;
  t->next_timer_id = 1;

  t->laters = NULL;

  return t;
}

static void tickit_destroy(Tickit *t)
{
  if(t->rootwin)
    tickit_window_unref(t->rootwin);
  if(t->term)
    tickit_term_unref(t->term);

  if(t->timers) {
    Deferral *this, *next;
    for(this = t->timers; this; this = next) {
      next = this->next;
      /* TODO: consider if there should be some sort of destroy invocation? */
      free(this);
    }
  }

  free(t);
}

Tickit *tickit_ref(Tickit *t)
{
  t->refcount++;
  return t;
}

void tickit_unref(Tickit *t)
{
  t->refcount--;
  if(!t->refcount)
    tickit_destroy(t);
}

TickitTerm *tickit_get_term(Tickit *t)
{
  if(!t->term) {
    TickitTerm *tt = tickit_term_open_stdio();
    if(!tt)
      return NULL;

    tickit_term_await_started_msec(tt, 50);

    tickit_term_setctl_int(tt, TICKIT_TERMCTL_ALTSCREEN, 1);
    tickit_term_setctl_int(tt, TICKIT_TERMCTL_CURSORVIS, 0);
    tickit_term_setctl_int(tt, TICKIT_TERMCTL_MOUSE, TICKIT_TERM_MOUSEMODE_DRAG);
    tickit_term_setctl_int(tt, TICKIT_TERMCTL_KEYPAD_APP, 1);

    tickit_term_clear(tt);

    t->term = tt;
  }

  return t->term;
}

TickitWindow *tickit_get_rootwin(Tickit *t)
{
  if(!t->rootwin) {
    TickitTerm *tt = tickit_get_term(t);
    if(!tt)
      return NULL;

    t->rootwin = tickit_window_new_root2(t, tt);
  }

  return t->rootwin;
}

// TODO: copy the entire SIGWINCH-like structure from term.c
// For now we only handle atmost-one running Tickit instance

static Tickit *running_tickit;

static void sigint(int sig)
{
  if(running_tickit)
    tickit_stop(running_tickit);
}

void tickit_run(Tickit *t)
{
  t->still_running = 1;

  running_tickit = t;
  signal(SIGINT, sigint);

  while(t->still_running) {
    int msec = -1;
    if(t->timers) {
      struct timeval now, delay;
      gettimeofday(&now, NULL);

      /* timers->at - now ==> delay */
      timersub(&t->timers->at, &now, &delay);

      msec = (delay.tv_sec * 1000) + (delay.tv_usec / 1000);
      if(msec < 0)
        msec = 0;
    }

    /* detach the later queue before running any events */
    Deferral *later = t->laters;
    t->laters = NULL;

    if(later)
      msec = 0;

    if(t->term)
      tickit_term_input_wait_msec(t->term, msec);
    /* else: er... handle msec somehow */

    if(t->timers) {
      struct timeval now;
      gettimeofday(&now, NULL);

      /* timer queue is stored ordered, so we can just eat a prefix
       * of it
       */

      Deferral *tim = t->timers;
      while(tim) {
        if(timercmp(&tim->at, &now, >))
          break;

        (*tim->fn)(t, tim->user);

        Deferral *next = tim->next;
        free(tim);
        tim = next;
      }

      t->timers = tim;
    }

    while(later) {
      (*later->fn)(t, later->user);

      Deferral *next = later->next;
      free(later);
      later = next;
    }
  }

  running_tickit = NULL;
}

void tickit_stop(Tickit *t)
{
  t->still_running = 0;
}

/* static for now until we decide how to expose it */
static int tickit_timer_at(Tickit *t, const struct timeval *at, TickitCallbackFn *fn, void *user)
{
  Deferral *tim = malloc(sizeof(Deferral));
  if(!tim)
    return -1;

  tim->next = NULL;

  tim->id = t->next_timer_id;
  tim->at = *at;
  tim->fn = fn;
  tim->user = user;

  t->next_timer_id++;

  Deferral **prevp = &t->timers;
  /* Try to insert in-order at matching timestamp */
  while(*prevp && !timercmp(&(*prevp)->at, at, >))
    prevp = &(*prevp)->next;

  tim->next = *prevp;
  *prevp = tim;

  return tim->id;
}

int tickit_timer_after_tv(Tickit *t, const struct timeval *after, TickitCallbackFn *fn, void *user)
{
  struct timeval at;
  gettimeofday(&at, NULL);

  /* at + after ==> at */
  timeradd(&at, after, &at);

  return tickit_timer_at(t, &at, fn, user);
}

int tickit_timer_after_msec(Tickit *t, int msec, TickitCallbackFn *fn, void *user)
{
  return tickit_timer_after_tv(t, &(struct timeval){
      .tv_sec = msec / 1000,
      .tv_usec = (msec % 1000) * 1000,
    }, fn, user);
}

void tickit_timer_cancel(Tickit *t, int id)
{
  Deferral **prevp = &t->timers;
  while(*prevp) {
    Deferral *this = *prevp;
    if(this->id == id) {
      *prevp = this->next;

      free(this);
    }

    prevp = &(*prevp)->next;
  }
}

int tickit_later(Tickit *t, TickitCallbackFn *fn, void *user)
{
  Deferral *later = malloc(sizeof(Deferral));
  if(!later)
    return -1;

  later->next = NULL;

  later->fn = fn;
  later->user = user;

  Deferral **prevp = &t->laters;
  while(*prevp)
    prevp = &(*prevp)->next;

  later->next = *prevp;
  *prevp = later;

  return 1;
}
