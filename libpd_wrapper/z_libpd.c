/*
 * Copyright (c) 2010 Peter Brinkmann (peter.brinkmann@gmail.com)
 *
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.
 *
 * See https://github.com/libpd/libpd/wiki for documentation
 *
 */

#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#ifndef LIBPD_NO_NUMERIC
# include <locale.h>
#endif
#include "z_libpd.h"
#include "x_libpdreceive.h"
#include "z_hooks.h"
#include "s_stuff.h"
#include "m_imp.h"
#include "g_all_guis.h"

#if PD_MINOR_VERSION < 46
# define HAVE_SCHED_TICK_ARG
#endif

#ifdef HAVE_SCHED_TICK_ARG
# define SCHED_TICK(x) sched_tick(x)
#else
# define SCHED_TICK(x) sched_tick()
#endif

// forward declares
void pd_init(void);
int sys_startgui(const char *libdir);
void sys_stopgui(void);
int sys_pollgui(void);

// (optional) built in pd externals setup functions
#ifdef LIBPD_EXTRA
  void bob_tilde_setup();
  void bonk_tilde_setup();
  void choice_setup();
  void fiddle_tilde_setup();
  void loop_tilde_setup();
  void lrshift_tilde_setup();
  void pique_setup();
  void sigmund_tilde_setup();
  void stdout_setup();
#endif

static PERTHREAD t_atom *argv = NULL;
static PERTHREAD t_atom *curr = NULL;
static PERTHREAD int argm = 0;
static PERTHREAD int argc = 0;

static void *get_object(const char *s) {
  t_pd *x = gensym(s)->s_thing;
  return x;
}

// this is called instead of sys_main() to start things
int libpd_init(void) {
  static int initialized = 0;
  if (initialized) return -1; // only allow init once (for now)
  initialized = 1;
  signal(SIGFPE, SIG_IGN);
  libpd_start_message(32); // allocate array for message assembly
  sys_printhook = (t_printhook) libpd_printhook;
  // are all these settings necessary?
  sys_externalschedlib = 0;
  sys_printtostderr = 0;
  sys_usestdpath = 0; // don't use pd_extrapath, only sys_searchpath
  sys_debuglevel = 0;
  sys_noloadbang = 0;
  sys_hipriority = 0;
  sys_nmidiin = 0;
  sys_nmidiout = 0;
#ifdef HAVE_SCHED_TICK_ARG
  sys_time = 0;
#endif
  pd_init();
  STUFF->st_soundin = NULL;
  STUFF->st_soundout = NULL;
  STUFF->st_schedblocksize = DEFDACBLKSIZE;
  sys_init_fdpoll();
  libpdreceive_setup();
  sys_set_audio_api(API_DUMMY);
  STUFF->st_searchpath = NULL;
#ifdef LIBPD_EXTRA
  bob_tilde_setup();
  bonk_tilde_setup();
  choice_setup();
  fiddle_tilde_setup();
  loop_tilde_setup();
  lrshift_tilde_setup();
  pique_setup();
  sigmund_tilde_setup();
  stdout_setup();
#endif
#ifndef LIBPD_NO_NUMERIC
  setlocale(LC_NUMERIC, "C");
#endif
  return 0;
}

void libpd_clear_search_path(void) {
  sys_lock();
  namelist_free(STUFF->st_searchpath);
  STUFF->st_searchpath = NULL;
  sys_unlock();
}

void libpd_add_to_search_path(const char *s) {
  sys_lock();
  STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, s, 0);
  sys_unlock();
}

void libpd_set_loglevel(int level) {
  sys_verbose = level;
}

int libpd_get_loglevel(void) {
  return sys_verbose;
}

void *libpd_openfile(const char *basename, const char *dirname) {
  void * retval;
  sys_lock();
  pd_globallock();
  retval = (void *)glob_evalfile(NULL, gensym(basename), gensym(dirname));
  pd_globalunlock();
  sys_unlock();
  return retval;
}

void libpd_closefile(void *x) {
  sys_lock();
  pd_free((t_pd *)x);
  sys_unlock();
}

int libpd_getdollarzero(void *x) {
  sys_lock();
  pd_pushsym((t_pd *)x);
  int dzero = canvas_getdollarzero();
  pd_popsym((t_pd *)x);
  sys_unlock();
  return dzero;
}

int libpd_init_audio(int inChans, int outChans, int sampleRate) {
  int indev[MAXAUDIOINDEV], inch[MAXAUDIOINDEV],
       outdev[MAXAUDIOOUTDEV], outch[MAXAUDIOOUTDEV];
  indev[0] = outdev[0] = DEFAULTAUDIODEV;
  inch[0] = inChans;
  outch[0] = outChans;
  sys_lock();
  sys_set_audio_settings(1, indev, 1, inch,
         1, outdev, 1, outch, sampleRate, -1, 1, DEFDACBLKSIZE);
  sched_set_using_audio(SCHED_AUDIO_CALLBACK);
  sys_reopen_audio();
  sys_unlock();
  return 0;
}

int libpd_process_raw(const float *inBuffer, float *outBuffer) {
  size_t n_in = STUFF->st_inchannels * DEFDACBLKSIZE;
  size_t n_out = STUFF->st_outchannels * DEFDACBLKSIZE;
  t_sample *p;
  size_t i;
  sys_lock();
  sys_microsleep(0);
  for (p = STUFF->st_soundin, i = 0; i < n_in; i++) {
    *p++ = *inBuffer++;
  }
  memset(STUFF->st_soundout, 0, n_out * sizeof(t_sample));
  SCHED_TICK(pd_this->pd_systime + STUFF->st_time_per_dsp_tick);
  for (p = STUFF->st_soundout, i = 0; i < n_out; i++) {
    *outBuffer++ = *p++;
  }
  sys_unlock();
  return 0;
}

static const t_sample sample_to_short = SHRT_MAX,
                   short_to_sample = 1.0 / (t_sample) SHRT_MAX;

#define PROCESS(_x, _y) \
  int i, j, k; \
  t_sample *p0, *p1; \
  sys_lock(); \
  sys_microsleep(0); \
  for (i = 0; i < ticks; i++) { \
    for (j = 0, p0 = STUFF->st_soundin; j < DEFDACBLKSIZE; j++, p0++) { \
      for (k = 0, p1 = p0; k < STUFF->st_inchannels; k++, p1 += DEFDACBLKSIZE) \
        { \
        *p1 = *inBuffer++ _x; \
      } \
    } \
    memset(STUFF->st_soundout, 0, \
        STUFF->st_outchannels*DEFDACBLKSIZE*sizeof(t_sample)); \
    SCHED_TICK(pd_this->pd_systime + STUFF->st_time_per_dsp_tick); \
    for (j = 0, p0 = STUFF->st_soundout; j < DEFDACBLKSIZE; j++, p0++) { \
      for (k = 0, p1 = p0; k < STUFF->st_outchannels; k++, p1 += DEFDACBLKSIZE) \
        { \
        *outBuffer++ = *p1 _y; \
      } \
    } \
  } \
  sys_unlock(); \
  return 0;

int libpd_process_short(int ticks, const short *inBuffer, short *outBuffer) {
  PROCESS(* short_to_sample, * sample_to_short)
}

int libpd_process_float(int ticks, const float *inBuffer, float *outBuffer) {
  PROCESS(,)
}

int libpd_process_double(int ticks, const double *inBuffer, double *outBuffer) {
  PROCESS(,)
}
 
#define GETARRAY \
  t_garray *garray = (t_garray *) pd_findbyclass(gensym(name), garray_class); \
  if (!garray) {sys_unlock(); return -1;} \

int libpd_arraysize(const char *name) {
  int retval;
  sys_lock();
  GETARRAY
  retval = garray_npoints(garray);
  sys_unlock();
  return retval;
}

#define MEMCPY(_x, _y) \
  GETARRAY \
  if (n < 0 || offset < 0 || offset + n > garray_npoints(garray)) return -2; \
  t_word *vec = ((t_word *) garray_vec(garray)) + offset; \
  int i; \
  for (i = 0; i < n; i++) _x = _y;

int libpd_read_array(float *dest, const char *name, int offset, int n) {
  sys_lock();
  MEMCPY(*dest++, (vec++)->w_float)
  sys_unlock();
  return 0;
}

int libpd_write_array(const char *name, int offset, float *src, int n) {
  sys_lock();
  MEMCPY((vec++)->w_float, *src++)
  sys_unlock();
  return 0;
}

void libpd_set_float(t_atom *v, float x) {
  SETFLOAT(v, x);
}

void libpd_set_symbol(t_atom *v, const char *sym) {
  SETSYMBOL(v, gensym(sym));
}

int libpd_list(const char *recv, int n, t_atom *v) {
  t_pd *dest;
  sys_lock();
  dest = get_object(recv);
  if (dest == NULL)
  {
    sys_unlock();
    return -1;
  }
  pd_list(dest, &s_list, n, v);
  sys_unlock();
  return 0;
}

int libpd_message(const char *recv, const char *msg, int n, t_atom *v) {
  t_pd *dest;
  sys_lock();
  dest = get_object(recv);
  if (dest == NULL)
  {
    sys_unlock();
    return -1;
  }
  pd_typedmess(dest, gensym(msg), n, v);
  sys_unlock();
  return 0;
}

int libpd_start_message(int max_length) {
  if (max_length > argm) {
    t_atom *v = realloc(argv, max_length * sizeof(t_atom));
    if (v) {
      argv = v;
      argm = max_length;
    } else {
      return -1;
    }
  }
  argc = 0;
  curr = argv;
  return 0;
}

#define ADD_ARG(f) f(curr, x); curr++; argc++;

void libpd_add_float(float x) {
  ADD_ARG(SETFLOAT);
}

void libpd_add_symbol(const char *s) {
  t_symbol *x;
  sys_lock();
  x = gensym(s);
  sys_unlock();
  ADD_ARG(SETSYMBOL);
}

int libpd_finish_list(const char *recv) {
  return libpd_list(recv, argc, argv);
}

int libpd_finish_message(const char *recv, const char *msg) {
  return libpd_message(recv, msg, argc, argv);
}

void *libpd_bind(const char *sym) {
  t_symbol *x;
  sys_lock();
  x = gensym(sym);
  sys_unlock();
  return libpdreceive_new(x);
}

void libpd_unbind(void *p) {
  sys_lock();
  pd_free((t_pd *)p);
  sys_unlock();
}

int libpd_is_float(t_atom *a) {
  return (a)->a_type == A_FLOAT;
}

int libpd_is_symbol(t_atom *a) {
  return (a)->a_type == A_SYMBOL;
}

float libpd_get_float(t_atom *a) {
  return (a)->a_w.w_float;
}

char *libpd_get_symbol(t_atom *a) {
  return (a)->a_w.w_symbol->s_name;
}

t_atom *libpd_next_atom(t_atom *a) {
  return a + 1;
}

void libpd_set_printhook(const t_libpd_printhook hook) {
  libpd_printhook = hook;
}

void libpd_set_banghook(const t_libpd_banghook hook) {
  libpd_banghook = hook;
}

void libpd_set_floathook(const t_libpd_floathook hook) {
  libpd_floathook = hook;
}

void libpd_set_symbolhook(const t_libpd_symbolhook hook) {
  libpd_symbolhook = hook;
}

void libpd_set_listhook(const t_libpd_listhook hook) {
  libpd_listhook = hook;
}

void libpd_set_messagehook(const t_libpd_messagehook hook) {
  libpd_messagehook = hook;
}

int libpd_symbol(const char *recv, const char *sym) {
  void *obj;
  sys_lock();
  obj = get_object(recv);
  if (obj == NULL)
  {
    sys_unlock();
    return -1;
  }
  pd_symbol(obj, gensym(sym));
  sys_unlock();
  return 0;
}

int libpd_float(const char *recv, float x) {
  void *obj;
  sys_lock();
  obj = get_object(recv);
  if (obj == NULL)
  {
    sys_unlock();
    return -1;
  }
  pd_float(obj, x);
  sys_unlock();
  return 0;
}

int libpd_bang(const char *recv) {
  void *obj;
  sys_lock();
  obj = get_object(recv);
  if (obj == NULL)
  {
    sys_unlock();
    return -1;
  }
  pd_bang(obj);
  sys_unlock();
  return 0;
}

int libpd_blocksize(void) {
  return DEFDACBLKSIZE;
}

int libpd_exists(const char *sym) {
  int retval;
  sys_lock();
  retval = (get_object(sym) != NULL);
  sys_unlock();
  return retval;
}

#define CHECK_CHANNEL if (channel < 0) return -1;
#define CHECK_PORT if (port < 0 || port > 0x0fff) return -1;
#define CHECK_RANGE_7BIT(v) if (v < 0 || v > 0x7f) return -1;
#define CHECK_RANGE_8BIT(v) if (v < 0 || v > 0xff) return -1;
#define PORT (channel >> 4)
#define CHANNEL (channel & 0x0f)

int libpd_noteon(int channel, int pitch, int velocity) {
  CHECK_CHANNEL
  CHECK_RANGE_7BIT(pitch)
  CHECK_RANGE_7BIT(velocity)
  sys_lock();
  inmidi_noteon(PORT, CHANNEL, pitch, velocity);
  sys_unlock();
  return 0;
}

int libpd_controlchange(int channel, int controller, int value) {
  CHECK_CHANNEL
  CHECK_RANGE_7BIT(controller)
  CHECK_RANGE_7BIT(value)
  sys_lock();
  inmidi_controlchange(PORT, CHANNEL, controller, value);
  sys_unlock();
  return 0;
}

int libpd_programchange(int channel, int value) {
  CHECK_CHANNEL
  CHECK_RANGE_7BIT(value)
  sys_lock();
  inmidi_programchange(PORT, CHANNEL, value);
  sys_unlock();
  return 0;
}

int libpd_pitchbend(int channel, int value) {
  CHECK_CHANNEL
  if (value < -8192 || value > 8191) return -1;
  sys_lock();
  inmidi_pitchbend(PORT, CHANNEL, value + 8192);
  sys_unlock();
  // Note: For consistency with Pd, we center the output of [pitchin]
  // at 8192.
  return 0;
}

int libpd_aftertouch(int channel, int value) {
  CHECK_CHANNEL
  CHECK_RANGE_7BIT(value)
  sys_lock();
  inmidi_aftertouch(PORT, CHANNEL, value);
  sys_unlock();
  return 0;
}

int libpd_polyaftertouch(int channel, int pitch, int value) {
  CHECK_CHANNEL
  CHECK_RANGE_7BIT(pitch)
  CHECK_RANGE_7BIT(value)
  sys_lock();
  inmidi_polyaftertouch(PORT, CHANNEL, pitch, value);
  sys_unlock();
  return 0;
}

int libpd_midibyte(int port, int byte) {
  CHECK_PORT
  CHECK_RANGE_8BIT(byte)
  sys_lock();
  inmidi_byte(port, byte);
  sys_unlock();
  return 0;
}

int libpd_sysex(int port, int byte) {
  CHECK_PORT
  CHECK_RANGE_8BIT(byte)
  sys_lock();
  inmidi_sysex(port, byte);
  sys_unlock();
  return 0;
}

int libpd_sysrealtime(int port, int byte) {
  CHECK_PORT
  CHECK_RANGE_8BIT(byte)
  sys_lock();
  inmidi_realtimein(port, byte);
  sys_unlock();
  return 0;
}

void libpd_set_noteonhook(const t_libpd_noteonhook hook) {
  libpd_noteonhook = hook;
}

void libpd_set_controlchangehook(const t_libpd_controlchangehook hook) {
  libpd_controlchangehook = hook;
}

void libpd_set_programchangehook(const t_libpd_programchangehook hook) {
  libpd_programchangehook = hook;
}

void libpd_set_pitchbendhook(const t_libpd_pitchbendhook hook) {
  libpd_pitchbendhook = hook;
}

void libpd_set_aftertouchhook(const t_libpd_aftertouchhook hook) {
  libpd_aftertouchhook = hook;
}

void libpd_set_polyaftertouchhook(const t_libpd_polyaftertouchhook hook) {
  libpd_polyaftertouchhook = hook;
}

void libpd_set_midibytehook(const t_libpd_midibytehook hook) {
  libpd_midibytehook = hook;
}

int libpd_start_gui(char *path) {
  int retval;
  sys_lock();
  retval = sys_startgui(path);
  sys_unlock();
  return retval;
}

void libpd_stop_gui(void) {
  sys_lock();
  sys_stopgui();
  sys_unlock();
}

void libpd_poll_gui(void) {
  sys_lock();
  sys_pollgui();
  sys_unlock();
}

t_pdinstance *libpd_new_instance(void) {
#ifdef PDINSTANCE
  return pdinstance_new();
#else
  return 0;
#endif
}

void libpd_set_instance(t_pdinstance *x) {
#ifdef PDINSTANCE
  pd_setinstance(x);
#endif
}

void libpd_free_instance(t_pdinstance *x) {
#ifdef PDINSTANCE
  pdinstance_free(x);
#endif
}

t_pdinstance *libpd_this_instance(void) {
  return pd_this;
}

t_pdinstance *libpd_get_instance(int index) {
#ifdef PDINSTANCE
  if(index < 0 || index >= pd_ninstances) {return 0;}
  return pd_instances[index];
#else
  return pd_this;
#endif
}

int libpd_num_instances(void) {
#ifdef PDINSTANCE
  return pd_ninstances;
#else
  return 1;
#endif
}

// dummy routines needed because we don't use s_file.c
void glob_loadpreferences(t_pd *dummy, t_symbol *s) {}
void glob_savepreferences(t_pd *dummy, t_symbol *s) {}
void glob_forgetpreferences(t_pd *dummy) {}
void sys_loadpreferences(const char *filename, int startingup) {}
int sys_oktoloadfiles(int done) {return 1;}
void sys_savepreferences(const char *filename) {} // used in s_path.c
