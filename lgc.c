/*
** $Id: lgc.c,v 2.4 2004/03/09 17:34:35 roberto Exp roberto $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#include <string.h>

#define lgc_c

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


#define GCSTEPSIZE	(40*sizeof(TValue))
#define GCFREECOST	(sizeof(TValue)/2)
#define GCSWEEPCOST	sizeof(TValue)


#define FIXEDMASK	bitmask(FIXEDBIT)

#define maskmarks \
	cast(lu_byte, ~(bitmask(BLACKBIT)|bit2mask(WHITE0BIT, WHITE1BIT)))

#define makewhite(g,x)	\
   ((x)->gch.marked = ((x)->gch.marked & maskmarks) | g->currentwhite)

#define white2gray(x)	reset2bits((x)->gch.marked, WHITE0BIT, WHITE1BIT)
#define gray2black(x)	setbit((x)->gch.marked, BLACKBIT)
#define black2gray(x)	resetbit((x)->gch.marked, BLACKBIT)

#define stringmark(s)	reset2bits((s)->tsv.marked, WHITE0BIT, WHITE1BIT)


#define isfinalized(u)		testbit((u)->marked, FINALIZEDBIT)
#define markfinalized(u)	setbit((u)->marked, FINALIZEDBIT)


#define KEYWEAK         bitmask(KEYWEAKBIT)
#define VALUEWEAK       bitmask(VALUEWEAKBIT)



#define markvalue(g,o) { checkconsistency(o); \
  if (iscollectable(o) && iswhite(gcvalue(o))) reallymarkobject(g,gcvalue(o)); }

#define markobject(g,t) { if (iswhite(obj2gco(t))) \
		reallymarkobject(g, obj2gco(t)); }



static void removeentry (Node *n) {
  setnilvalue(gval(n));  /* remove corresponding value ... */
  if (iscollectable(gkey(n)))
    setttype(gkey(n), LUA_TNONE);  /* dead key; remove it */
}


static void reallymarkobject (global_State *g, GCObject *o) {
  lua_assert(iswhite(o) && !isdead(g, o));
  white2gray(o);
  switch (o->gch.tt) {
    case LUA_TSTRING: {
      return;
    }
    case LUA_TUSERDATA: {
      Table *mt = gco2u(o)->metatable;
      gray2black(o);  /* udata are never gray */
      if (mt) markobject(g, mt);
      return;
    }
    case LUA_TUPVAL: {
      UpVal *uv = gco2uv(o);
      if (uv->v == &uv->value) {  /* closed? */
        markvalue(g, uv->v);
        gray2black(o);
      }
      return;
    }
    case LUA_TFUNCTION: {
      gco2cl(o)->c.gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TTABLE: {
      gco2h(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TTHREAD: {
      gco2th(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
    case LUA_TPROTO: {
      gco2p(o)->gclist = g->gray;
      g->gray = o;
      break;
    }
    default: lua_assert(0);
  }
}


static void marktmu (global_State *g) {
  GCObject *u;
  for (u = g->tmudata; u; u = u->gch.next) {
    makewhite(g, u);  /* may be marked, if left from previous GC */
    reallymarkobject(g, u);
  }
}


/* move `dead' udata that need finalization to list `tmudata' */
size_t luaC_separateudata (lua_State *L, int all) {
  size_t deadmem = 0;
  GCObject **p = &G(L)->firstudata;
  GCObject *curr;
  GCObject *collected = NULL;  /* to collect udata with gc event */
  GCObject **lastcollected = &collected;
  while ((curr = *p)->gch.tt == LUA_TUSERDATA) {
    if (!(iswhite(curr) || all) || isfinalized(gco2u(curr)))
      p = &curr->gch.next;  /* don't bother with them */
    else if (fasttm(L, gco2u(curr)->metatable, TM_GC) == NULL) {
      markfinalized(gco2u(curr));  /* don't need finalization */
      p = &curr->gch.next;
    }
    else {  /* must call its gc method */
      deadmem += sizeudata(gco2u(curr)->len);
      markfinalized(gco2u(curr));
      *p = curr->gch.next;
      curr->gch.next = NULL;  /* link `curr' at the end of `collected' list */
      *lastcollected = curr;
      lastcollected = &curr->gch.next;
    }
  }
  lua_assert(curr == obj2gco(G(L)->mainthread));
  /* insert collected udata with gc event into `tmudata' list */
  *lastcollected = G(L)->tmudata;
  G(L)->tmudata = collected;
  return deadmem;
}


static int traversetable (global_State *g, Table *h) {
  int i;
  int weakkey = 0;
  int weakvalue = 0;
  const TValue *mode;
  if (h->metatable)
    markobject(g, h->metatable);
  lua_assert(h->lsizenode || h->node == g->dummynode);
  mode = gfasttm(g, h->metatable, TM_MODE);
  if (mode && ttisstring(mode)) {  /* is there a weak mode? */
    weakkey = (strchr(svalue(mode), 'k') != NULL);
    weakvalue = (strchr(svalue(mode), 'v') != NULL);
    if (weakkey || weakvalue) {  /* is really weak? */
      h->marked &= ~(KEYWEAK | VALUEWEAK);  /* clear bits */
      h->marked |= cast(lu_byte, (weakkey << KEYWEAKBIT) |
                                 (weakvalue << VALUEWEAKBIT));
      h->gclist = g->weak;  /* must be cleared after GC, ... */
      g->weak = obj2gco(h);  /* ... so put in the appropriate list */
    }
  }
  if (weakkey && weakvalue) return 1;
  if (!weakvalue) {
    i = h->sizearray;
    while (i--)
      markvalue(g, &h->array[i]);
  }
  i = sizenode(h);
  while (i--) {
    Node *n = gnode(h, i);
    if (ttisnil(gval(n)))
      removeentry(n);  /* remove empty entries */
    else {
      lua_assert(!ttisnil(gkey(n)));
      if (!weakkey) markvalue(g, gkey(n));
      if (!weakvalue) markvalue(g, gval(n));
    }
  }
  return weakkey || weakvalue;
}


/*
** All marks are conditional because a GC may happen while the
** prototype is still being created
*/
static void traverseproto (global_State *g, Proto *f) {
  int i;
  if (f->source) stringmark(f->source);
  for (i=0; i<f->sizek; i++)  /* mark literals */
    markvalue(g, &f->k[i]);
  for (i=0; i<f->sizeupvalues; i++) {  /* mark upvalue names */
    if (f->upvalues[i])
      stringmark(f->upvalues[i]);
  }
  for (i=0; i<f->sizep; i++) {  /* mark nested protos */
    if (f->p[i])
      markobject(g, f->p[i]);
  }
  for (i=0; i<f->sizelocvars; i++) {  /* mark local-variable names */
    if (f->locvars[i].varname)
      stringmark(f->locvars[i].varname);
  }
}



static void traverseclosure (global_State *g, Closure *cl) {
  if (cl->c.isC) {
    int i;
    for (i=0; i<cl->c.nupvalues; i++)  /* mark its upvalues */
      markvalue(g, &cl->c.upvalue[i]);
  }
  else {
    int i;
    lua_assert(cl->l.nupvalues == cl->l.p->nups);
    markobject(g, hvalue(&cl->l.g));
    markobject(g, cl->l.p);
    for (i=0; i<cl->l.nupvalues; i++)  /* mark its upvalues */
      markobject(g, cl->l.upvals[i]);
  }
}


static void checkstacksizes (lua_State *L, StkId max) {
  int used = L->ci - L->base_ci;  /* number of `ci' in use */
  if (4*used < L->size_ci && 2*BASIC_CI_SIZE < L->size_ci)
    luaD_reallocCI(L, L->size_ci/2);  /* still big enough... */
  else condhardstacktests(luaD_reallocCI(L, L->size_ci));
  used = max - L->stack;  /* part of stack in use */
  if (4*used < L->stacksize && 2*(BASIC_STACK_SIZE+EXTRA_STACK) < L->stacksize)
    luaD_reallocstack(L, L->stacksize/2);  /* still big enough... */
  else condhardstacktests(luaD_reallocstack(L, L->stacksize));
}


static void traversestack (global_State *g, lua_State *l) {
  StkId o, lim;
  CallInfo *ci;
  markvalue(g, gt(l));
  lim = l->top;
  for (ci = l->base_ci; ci <= l->ci; ci++) {
    lua_assert(ci->top <= l->stack_last);
    if (lim < ci->top) lim = ci->top;
  }
  for (o = l->stack; o < l->top; o++)
    markvalue(g, o);
  for (; o <= lim; o++)
    setnilvalue(o);
  checkstacksizes(l, lim);
}


/*
** traverse a given `quantity' of gray objects,
** turning them to black. Returns extra `quantity' traversed.
*/
static l_mem propagatemarks (global_State *g, l_mem lim) {
  GCObject *o;
  while ((o = g->gray) != NULL) {
    lua_assert(isgray(o));
    gray2black(o);
    switch (o->gch.tt) {
      case LUA_TTABLE: {
        Table *h = gco2h(o);
        g->gray = h->gclist;
        if (traversetable(g, h))  /* table is weak? */
          black2gray(o);  /* keep it gray */
        lim -= sizeof(Table) + sizeof(TValue) * h->sizearray +
                               sizeof(Node) * sizenode(h);
        break;
      }
      case LUA_TFUNCTION: {
        Closure *cl = gco2cl(o);
        g->gray = cl->c.gclist;
        traverseclosure(g, cl);
        lim -= (cl->c.isC) ? sizeCclosure(cl->c.nupvalues) :
                             sizeLclosure(cl->l.nupvalues);
        break;
      }
      case LUA_TTHREAD: {
        lua_State *th = gco2th(o);
        g->gray = th->gclist;
        th->gclist = g->grayagain;
        g->grayagain = o;
        black2gray(o);
        traversestack(g, th);
        lim -= sizeof(lua_State) + sizeof(TValue) * th->stacksize +
                                   sizeof(CallInfo) * th->size_ci;
        break;
      }
      case LUA_TPROTO: {
        Proto *p = gco2p(o);
        g->gray = p->gclist;
        traverseproto(g, p);
        lim -= sizeof(Proto) + sizeof(Instruction) * p->sizecode +
                               sizeof(Proto *) * p->sizep +
                               sizeof(TValue) * p->sizek + 
                               sizeof(int) * p->sizelineinfo +
                               sizeof(LocVar) * p->sizelocvars +
                               sizeof(TString *) * p->sizeupvalues;
        break;
      }
      default: lua_assert(0);
    }
    if (lim <= 0) return lim;
  }
  return lim;
}


/*
** The next function tells whether a key or value can be cleared from
** a weak table. Non-collectable objects are never removed from weak
** tables. Strings behave as `values', so are never removed too. for
** other objects: if really collected, cannot keep them; for userdata
** being finalized, keep them in keys, but not in values
*/
static int iscleared (const TValue *o, int iskey) {
  if (!iscollectable(o)) return 0;
  if (ttisstring(o)) {
    stringmark(rawtsvalue(o));  /* strings are `values', so are never weak */
    return 0;
  }
  return iswhite(gcvalue(o)) ||
    (ttisuserdata(o) && (!iskey && isfinalized(uvalue(o))));
}


/*
** clear collected entries from weaktables
*/
static void cleartable (GCObject *l) {
  while (l) {
    Table *h = gco2h(l);
    int i = h->sizearray;
    lua_assert(testbit(h->marked, VALUEWEAKBIT) ||
               testbit(h->marked, KEYWEAKBIT));
    if (testbit(h->marked, VALUEWEAKBIT)) {
      while (i--) {
        TValue *o = &h->array[i];
        if (iscleared(o, 0))  /* value was collected? */
          setnilvalue(o);  /* remove value */
      }
    }
    i = sizenode(h);
    while (i--) {
      Node *n = gnode(h, i);
      if (!ttisnil(gval(n)) &&  /* non-empty entry? */
          (iscleared(gkey(n), 1) || iscleared(gval(n), 0)))
        removeentry(n);  /* remove entry from table */
    }
    l = h->gclist;
  }
}


static void freeobj (lua_State *L, GCObject *o) {
  switch (o->gch.tt) {
    case LUA_TPROTO: luaF_freeproto(L, gco2p(o)); break;
    case LUA_TFUNCTION: luaF_freeclosure(L, gco2cl(o)); break;
    case LUA_TUPVAL: luaM_freelem(L, gco2uv(o)); break;
    case LUA_TTABLE: luaH_free(L, gco2h(o)); break;
    case LUA_TTHREAD: {
      lua_assert(gco2th(o) != L && gco2th(o) != G(L)->mainthread);
      luaE_freethread(L, gco2th(o));
      break;
    }
    case LUA_TSTRING: {
      luaM_free(L, o, sizestring(gco2ts(o)->len));
      break;
    }
    case LUA_TUSERDATA: {
      luaM_free(L, o, sizeudata(gco2u(o)->len));
      break;
    }
    default: lua_assert(0);
  }
}


static void sweepupvalues (global_State *g, lua_State *l) {
  GCObject *curr;
  for (curr = l->openupval; curr != NULL; curr = curr->gch.next)
    makewhite(g, curr);
}


/*
** macros to test dead bit and optionally the fix bit
*/
#define makedeadmask(g,kf)	(otherwhite(g) | ((kf) ? FIXEDMASK : 0))
#define notdead(mark,mask)	((((mark) ^ FIXEDMASK) & mask) != mask)


static GCObject **sweeplist (lua_State *L, GCObject **p, int keepfixed,
                             l_mem *plim) {
  GCObject *curr;
  global_State *g = G(L);
  l_mem lim = *plim;
  int deadmask = makedeadmask(g, keepfixed);
  while ((curr = *p) != NULL) {
    if (notdead(curr->gch.marked, deadmask)) {
      makewhite(g, curr);
      if (curr->gch.tt == LUA_TTHREAD)
        sweepupvalues(g, gco2th(curr));
      p = &curr->gch.next;
      lim -= GCSWEEPCOST;
    }
    else {
      lua_assert(iswhite(curr));
      *p = curr->gch.next;
      if (curr == g->rootgc)  /* is the first element of the list? */
        g->rootgc = curr->gch.next;  /* adjust first */
      freeobj(L, curr);
      lim -= GCFREECOST;
    }
    if (lim <= 0) break;
  }
  *plim = lim;
  return p;
}


static l_mem sweepstrings (lua_State *L, int keepfixed, l_mem lim) {
  int i;
  global_State *g = G(L);
  int deadmask = makedeadmask(g, keepfixed);
  for (i = g->sweepstrgc; i < g->strt.size; i++) {  /* for each list */
    GCObject *curr;
    GCObject **p = &G(L)->strt.hash[i];
    while ((curr = *p) != NULL) {
      lu_mem size = sizestring(gco2ts(curr)->len);
      if (notdead(curr->gch.marked, deadmask)) {
        makewhite(g, curr);
        lua_assert(iswhite(curr) && !isdead(g, curr));
        p = &curr->gch.next;
      }
      else {
        lua_assert(iswhite(curr));
        g->strt.nuse--;
        *p = curr->gch.next;
        luaM_free(L, curr, size);
      }
      lim -= size;
    }
    if (lim <= 0) break;
  }
  g->sweepstrgc = i+1;
  return lim;
}


static void checkSizes (lua_State *L) {
  global_State *g = G(L);
  /* check size of string hash */
  if (g->strt.nuse < cast(lu_int32, G(L)->strt.size/4) &&
      g->strt.size > MINSTRTABSIZE*2)
    luaS_resize(L, g->strt.size/2);  /* table is too big */
  /* check size of buffer */
  if (luaZ_sizebuffer(&g->buff) > LUA_MINBUFFER*2) {  /* buffer too big? */
    size_t newsize = luaZ_sizebuffer(&g->buff) / 2;
    luaZ_resizebuffer(L, &g->buff, newsize);
  }
}


static void GCTM (lua_State *L) {
  global_State *g = G(L);
  GCObject *o = g->tmudata;
  Udata *udata = rawgco2u(o);
  const TValue *tm;
  g->tmudata = udata->uv.next;  /* remove udata from `tmudata' */
  udata->uv.next = g->firstudata->uv.next;  /* return it to `root' list */
  g->firstudata->uv.next = o;
  makewhite(g, o);
  tm = fasttm(L, udata->uv.metatable, TM_GC);
  if (tm != NULL) {
    lu_byte oldah = L->allowhook;
    L->allowhook = 0;  /* stop debug hooks during GC tag method */
    setobj2s(L, L->top, tm);
    setuvalue(L, L->top+1, udata);
    L->top += 2;
    luaD_call(L, L->top - 2, 0);
    L->allowhook = oldah;  /* restore hooks */
  }
}


/*
** Call all GC tag methods
*/
void luaC_callGCTM (lua_State *L) {
  while (G(L)->tmudata)
    GCTM(L);
}


void luaC_sweepall (lua_State *L) {
  global_State *g = G(L);
  l_mem dummy = MAXLMEM;
  /* finish (occasional) current sweep */
  markobject(g, g->mainthread);  /* cannot collect main thread */
  sweepstrings(L, 0, MAXLMEM);
  sweeplist(L, &g->rootgc, 0, &dummy);
  /* do a whole new sweep */
  markobject(g, g->mainthread);  /* cannot collect main thread */
  g->currentwhite = otherwhite(g);
  g->sweepgc = &g->rootgc;
  g->sweepstrgc = 0;
  sweepstrings(L, 0, MAXLMEM);
  sweeplist(L, &g->rootgc, 0, &dummy);
}


/* mark root set */
static void markroot (lua_State *L) {
  global_State *g = G(L);
  lua_assert(g->gray == NULL);
  g->grayagain = NULL;
  g->weak = NULL;
  makewhite(g, obj2gco(g->mainthread));
  markobject(g, g->mainthread);
  markvalue(g, registry(L));
  markobject(g, L);  /* mark running thread */
  g->gcstate = GCSpropagate;
}


static void remarkupvals (global_State *g) {
  GCObject *o;
  for (o = obj2gco(g->mainthread); o; o = o->gch.next) {
    if (iswhite(o)) {
      GCObject *curr;
      for (curr = gco2th(o)->openupval; curr != NULL; curr = curr->gch.next) {
        if (isgray(curr))
          markvalue(g, gco2uv(curr)->v);
      }
    }
  }
}


static void atomic (lua_State *L) {
  global_State *g = G(L);
  lua_assert(g->gray == NULL);
  /* remark occasional upvalues of (maybe) dead threads */
  remarkupvals(g);
  /* remark weak tables */
  g->gray = g->weak;
  g->weak = NULL;
  lua_assert(!iswhite(obj2gco(g->mainthread)));
  markobject(g, L);  /* mark running thread */
  propagatemarks(g, MAXLMEM);
  /* remark gray again */
  g->gray = g->grayagain;
  g->grayagain = NULL;
  propagatemarks(g, MAXLMEM);
  luaC_separateudata(L, 0);  /* separate userdata to be preserved */
  marktmu(g);  /* mark `preserved' userdata */
  propagatemarks(g, MAXLMEM);  /* remark, to propagate `preserveness' */
  cleartable(g->weak);  /* remove collected objects from weak tables */
  /* flip current white */
  g->currentwhite = otherwhite(g);
  g->gcstate = GCSsweepstring;
}


static l_mem singlestep (lua_State *L, l_mem lim) {
  global_State *g = G(L);
  switch (g->gcstate) {
    case GCSpropagate: {
      if (g->gray)
        lim = propagatemarks(g, lim);
      else {  /* no more `gray' objects */
        atomic(L);  /* finish mark phase */
        lim = 0;
      }
      break;
    }
    case GCSsweepstring: {
      lim = sweepstrings(L, 1, lim);
      if (g->sweepstrgc >= g->strt.size) {  /* nothing more to sweep? */
        g->sweepstrgc = 0;
        g->gcstate = GCSsweep;  /* end sweep-string phase */
      }
      break;
    }
    case GCSsweep: {
      g->sweepgc = sweeplist(L, g->sweepgc, 1, &lim);
      if (*g->sweepgc == NULL) {  /* nothing more to sweep? */
        checkSizes(L);
        g->sweepgc = &g->rootgc;
        g->gcstate = GCSfinalize;  /* end sweep phase */
      }
      break;
    }
    case GCSfinalize: {
      if (g->tmudata)
        GCTM(L);
      else  /* no more `udata' to finalize */
        markroot(L);  /* may restart collection */
      lim = 0;
      break;
    }
    default: lua_assert(0);
  }
  return lim;
}


void luaC_step (lua_State *L) {
  global_State *g = G(L);
  l_mem lim = (g->nblocks - (g->GCthreshold - GCSTEPSIZE)) * 2;
/*printf("+ %d %lu %lu %ld\n", g->gcstate, g->nblocks, g->GCthreshold, lim);*/
  while (lim > 0) lim = singlestep(L, lim);
  g->GCthreshold = g->nblocks + GCSTEPSIZE - lim/2;
/*printf("- %d %lu %lu %ld\n", g->gcstate, g->nblocks, g->GCthreshold, lim);*/
  lua_assert((long)g->nblocks + (long)GCSTEPSIZE >= lim/2);
}


void luaC_fullgc (lua_State *L) {
  global_State *g = G(L);
  while (g->gcstate != GCSfinalize) {
    singlestep(L, MAXLMEM);
  }
  markroot(L);
  while (g->gcstate != GCSfinalize) {
    singlestep(L, MAXLMEM);
  }
  g->GCthreshold = g->nblocks + GCSTEPSIZE;
  luaC_callGCTM(L);  /* call finalizers */
}


void luaC_barrierf (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  lua_assert(g->gcstate != GCSfinalize);
  if (g->gcstate != GCSpropagate)  /* sweeping phases? */
    black2gray(o);  /* just mark as gray to avoid other barriers */
  else  /* breaking invariant! */
    reallymarkobject(g, v);  /* restore it */
}


void luaC_link (lua_State *L, GCObject *o, lu_byte tt) {
  global_State *g = G(L);
  o->gch.next = g->rootgc;
  g->rootgc = o;
  o->gch.marked = luaC_white(g);
  o->gch.tt = tt;
}


void luaC_linkupval (lua_State *L, UpVal *uv) {
  global_State *g = G(L);
  GCObject *o = obj2gco(uv);
  o->gch.next = g->rootgc;  /* link upvalue into `rootgc' list */
  g->rootgc = o;
  if (isgray(o)) { 
    if (g->gcstate == GCSpropagate) {
      gray2black(o);  /* closed upvalues need barrier */
      luaC_barrier(L, uv, uv->v);
    }
    else {  /* sweep phase: sweep it (turning it into white) */
      makewhite(g, o);
      lua_assert(g->gcstate != GCSfinalize);
    }
  }
}

