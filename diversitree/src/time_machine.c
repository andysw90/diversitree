#include <R.h>
#include <Rinternals.h>
#include "util-splines.h"
#include "util.h"

#include "time_machine.h"

/* 1. Here are possible types of functions, and their constants */
#define T_CONSTANT 0
#define T_LINEAR   1
#define T_STEPF    2
#define T_SIGMOID  3
#define T_SPLINE   4

double t_linear(double t, double *p) {
  return p[0] + t * p[1];
}
double t_stepf(double t, double *p) {
  return t <= p[2] ? p[0] : p[1];
}
double t_sigmoid(double t, double *p) {
  //     y0      y1     y0             r       tmid
  return p[0] + (p[1] - p[0])/(1 + exp(p[4] * (p[3] - t)));
}
double t_spline(double t, double *p, dt_spline *obj) {
  //y0    y1     y0
  return p[0] + (p[1] - p[0]) * dt_spline_eval1(obj, t);
}

/* A little helper function to order these in R so I don't have to
   rely on remembering */
SEXP r_get_time_machine_types() {
  SEXP ret;
  PROTECT(ret = allocVector(STRSXP, 5));
  SET_STRING_ELT(ret, T_CONSTANT, mkChar("constant.t"));
  SET_STRING_ELT(ret, T_LINEAR,   mkChar("linear.t"));
  SET_STRING_ELT(ret, T_STEPF,    mkChar("stepf.t"));
  SET_STRING_ELT(ret, T_SIGMOID,  mkChar("sigmoid.t"));
  SET_STRING_ELT(ret, T_SPLINE,   mkChar("spline.t"));
  UNPROTECT(1);
  return ret;
}

/* 2. The time machine itself */

/* Make/cleanup function */
static void dt_time_machine_finalize(SEXP extPtr);
SEXP r_make_time_machine(SEXP obj) {
  SEXP extPtr;
  /* Extract stored R objects to build the time machine */
  SEXP
    types           = getListElement(obj, "types"),
    start           = getListElement(obj, "start"),
    nonnegative     = getListElement(obj, "nonnegative"),
    t_range         = getListElement(obj, "t.range"),
    spline_data     = getListElementIfThere(obj, "spline.data"),
    q_info          = getListElementIfThere(obj, "q.info");
  int np_in = INTEGER(getListElement(obj, "np.in"))[0],
    np_out  = INTEGER(getListElement(obj, "np.out"))[0],
    nf = LENGTH(types);
  int k, idx_q;
  dt_time_machine *ret;

  ret = (dt_time_machine *)Calloc(1, dt_time_machine);
  ret->np_in  = np_in;
  ret->p_in   = Calloc(np_in, double);
  ret->np_out = np_out;
  ret->p_out   = Calloc(np_out, double);
  ret->nf      = nf;

  ret->types       = Calloc(nf, int);
  ret->start       = Calloc(nf, int);
  ret->nonnegative = Calloc(nf, int);
  memcpy(ret->types,       INTEGER(types),       nf*sizeof(int));
  memcpy(ret->start,       INTEGER(start),       nf*sizeof(int));
  memcpy(ret->nonnegative, LOGICAL(nonnegative), nf*sizeof(int));
  memcpy(ret->t_range,     REAL(t_range),         2*sizeof(double));

  if ( spline_data != R_NilValue )
    ret->spline_data = 
      make_dt_spline(LENGTH(VECTOR_ELT(spline_data, 0)),
		     REAL(VECTOR_ELT(spline_data, 0)),
		     REAL(VECTOR_ELT(spline_data, 1)),
		     INTEGER(VECTOR_ELT(spline_data, 2))[0]);

  if ( q_info == R_NilValue ) {
    ret->k = 0;
  } else {
    k = ret->k = INTEGER(getListElement(q_info, "k"))[0];
    idx_q      = INTEGER(getListElement(q_info, "idx.q"))[0];
    memcpy(ret->q_const,  INTEGER(getListElement(q_info, "q_const")),
	   k * (k - 1)*sizeof(int));
    memcpy(ret->q_target, INTEGER(getListElement(q_info, "q_target")),
	   k * (k - 1)*sizeof(int));
    ret->q_in  = ret->p_in  + idx_q;
    ret->q_out = ret->p_out + idx_q;
  }

  /* And, done */
  extPtr = R_MakeExternalPtr(ret, R_NilValue, R_NilValue);
  R_RegisterCFinalizer(extPtr, dt_time_machine_finalize);
  return extPtr;
}

void cleanup_time_machine(dt_time_machine *obj) {
  Free(obj->p_in);
  Free(obj->p_out);
  Free(obj->types);
  Free(obj->start);
  Free(obj->nonnegative);
  if ( obj->spline_data != NULL )
    cleanup_dt_spline(obj->spline_data);
  Free(obj);
}

static void dt_time_machine_finalize(SEXP extPtr) {
  dt_time_machine *obj = (dt_time_machine*)R_ExternalPtrAddr(extPtr);
  cleanup_time_machine(obj);
}

/* When initialising a time machine, we set all constant arguments to
   their current value 
   
   It's possible that we can actually do better here; where (say) the
   function is linear and the slope is exactly zero, we can skip this.
   There are a limited enough range of options that this might be
   simple enough to implement and fast enough to warrent implementing.
   
   Furthermore looping over an array of indices
   (which(type!=constant)) will possibly be faster when most things
   aren't variable.
   
   ** TODO **
   check to see if pars is the same as previous
   memcmp(pars, obj->p_in)
   and if so just skip updating?
*/
void init_time_machine(dt_time_machine *obj, double *pars) {
  double *p_out = obj->p_out, *pi;
  const int np_in = obj->np_in, nf = obj->nf;
  const int *types = obj->types, *start = obj->start,
    *nonnegative = obj->nonnegative;
  double *t_range = obj->t_range;
  int i, k = obj->k;

  /* First, check negativity */
  for ( i = 0; i < nf; i++ ) {
    if ( nonnegative[i] ) {
      pi = pars + start[i];
      switch (types[i]) {
      case T_CONSTANT:
	if ( pi[0] < 0 )
	  error("Negative parameter in constant parameter");
	break;
      case T_LINEAR:
	if ( pi[0] + pi[1] * t_range[0] < 0 ||
	     pi[0] + pi[1] * t_range[1] < 0)
	  error("Negative parameter in linear function");
	break;
      case T_STEPF:   /* drops through */
      case T_SIGMOID: /* drops through */
      case T_SPLINE:
	if ( pi[0] < 0 || pi[1] < 0 )
	  error("Negative parameter in step/sigmoid/spline parameter");	
	break;
      }
    }
  }

  /* First, store all parameters */
  memcpy(obj->p_in, pars, np_in*sizeof(double));

  /* Then copy in all constant values */
  for ( i = 0; i < nf; i++ )
    if ( types[i] == T_CONSTANT )
      p_out[target[i]] = pars[start[i]];

  if ( k > 0 )
    normalise_q(obj, 1);
}

void normalise_q(dt_time_machine *obj, int is_const) {
  int k = obj->k;
  double *q_out = obj->q_out;
  double *qi_out, tmp;

  for ( i = 0; i < k; i++ ) {
    if ( obj->q_const[i] == is_const ) {
      qi_out = q_out + i;
      tmp = 0;
      for ( j = 0; j < k; j++ )
	if ( j != i )
	  tmp += q_out[j];
      qi_out[j] = tmp;
    }
  }
}

/* Compute values at a time 't' */
void run_time_machine(dt_time_machine *obj, double t) {
  double *p_in = obj->p_in, *p_out = obj->p_out;
  const int nf = obj->nf, *types = obj->types, *start = obj->start;
  int i;
  for ( i = 0; i < nf; i++ )
    switch (types[i]) {
    case T_LINEAR: 
      p_out[i] = t_linear(t, p_in + start[i]);
      break;
    case T_STEPF:
      p_out[i] = t_stepf(t, p_in + start[i]);
      break;
    case T_SIGMOID:
      p_out[i] = t_sigmoid(t, p_in + start[i]);
      break;
    case T_SPLINE:
      p_out[i] = t_spline(t, p_in + start[i], obj->spline_data);
      break;
    }
}

/* R interface to the above functions */
SEXP r_init_time_machine(SEXP extPtr, SEXP pars) {
  init_time_machine((dt_time_machine*)R_ExternalPtrAddr(extPtr),
		    REAL(pars));
  return R_NilValue;
}

SEXP r_run_time_machine(SEXP extPtr, SEXP t) {
  dt_time_machine *obj = (dt_time_machine*)R_ExternalPtrAddr(extPtr);
  SEXP ret;
  run_time_machine(obj, REAL(t)[0]);
  PROTECT(ret = allocVector(REALSXP, obj->np_out));
  memcpy(REAL(ret), obj->p_out, obj->np_out*sizeof(double));
  UNPROTECT(1);
  return ret;
}

/* Not sure if this one is ever useful */
SEXP r_get_time_machine_pars(SEXP extPtr) {
  dt_time_machine *obj = (dt_time_machine*)R_ExternalPtrAddr(extPtr);
  SEXP ret;
  PROTECT(ret = allocVector(REALSXP, obj->np_out));
  memcpy(REAL(ret), obj->p_out, obj->np_out*sizeof(double));
  UNPROTECT(1);
  return ret;
}
