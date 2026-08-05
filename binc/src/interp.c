/* interp.c — CPU reference interpreter for the scalar/vector/math subset.
 *
 * Semantics follow the SPEC: sequential execution of one kernel over a fixed
 * grid (no barriers, atomics, threadgroup memory, textures, or render stages —
 * SIMT behavior cannot be reproduced on the CPU). Device buffers are zeroed
 * 256-word arrays; after the run the interpreter prints buffer 0 so tests can
 * compare (`binc -i file.binc`). */
#include "binc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IBUFSZ 256
#define MAX_DEPTH 64
#define NANS 0

/* interpreted value: floats live in f[], ints in i[]; vecn lanes */
typedef struct { TypeKind k; int vecn; double f[4]; long long i[4]; } IV;
typedef struct { const char *name; IV v; IV *arr; int arrn; } ILoc;
typedef struct { const Program *prog; Function *fn; ILoc *locs; int nlocs, lcap;
    IV *bufs; int nbufs; int grid, cur, depth; IV ret; int has_ret; } ICtx;
typedef enum { FL_NONE, FL_BREAK, FL_CONT, FL_RET } Flow;

static IV iv_num(TypeKind k, double f){ IV v={0}; v.k=k; v.vecn=0; v.f[0]=f; v.i[0]=(long long)f; return v; }
static IV iv_i(TypeKind k, long long x){ IV v={0}; v.k=k; v.vecn=0; v.i[0]=x; v.f[0]=(double)x; return v; }
static IV iv_fv(TypeKind k, int n, const double *f){ IV v={0}; v.k=k; v.vecn=n; for(int i=0;i<n;i++){v.f[i]=f[i]; v.i[i]=(long long)f[i];} return v; }
static IV iv_iv(TypeKind k, int n, const long long *x){ IV v={0}; v.k=k; v.vecn=n; for(int i=0;i<n;i++){v.i[i]=x[i]; v.f[i]=(double)x[i];} return v; }
static int iv_int(IV v){ return v.k==T_FLOAT||v.k==T_HALF ? (int)v.f[0] : (int)(v.i[0]&0xFFFFFFFF); }
static double iv_flt(IV v){ return v.k==T_FLOAT||v.k==T_HALF ? v.f[0] : (double)(int)(v.i[0]&0xFFFFFFFF); }
static int is_float_k(TypeKind k){ return k==T_FLOAT||k==T_HALF; }
static IV iv_zero(TypeKind k, int n){ IV v={0}; v.k=k; v.vecn=n; return v; }

static int iloc_find(ICtx *c, const char *name){ for(int i=0;i<c->nlocs;i++) if(!strcmp(c->locs[i].name,name)) return i; return -1; }
static void iloc_set(ICtx *c, const char *name, IV v){ int i=iloc_find(c,name); if(i<0){ if(c->nlocs==c->lcap){c->lcap=c->lcap?c->lcap*2:16; c->locs=realloc(c->locs,c->lcap*sizeof(ILoc));} i=c->nlocs++; c->locs[i].name=name; c->locs[i].arr=NULL; c->locs[i].arrn=0; } c->locs[i].v=v; }
static void iloc_arr(ICtx *c, const char *name, int n){ if(c->nlocs==c->lcap){c->lcap=c->lcap?c->lcap*2:16; c->locs=realloc(c->locs,c->lcap*sizeof(ILoc));} int i=c->nlocs++; c->locs[i].name=name; c->locs[i].v=iv_zero(T_FLOAT,0); c->locs[i].arr=calloc((size_t)n,sizeof(IV)); c->locs[i].arrn=n; }

static void ibuf_set(ICtx *c, int pi, long long idx, IV v){
    if(idx<0||idx>=IBUFSZ){ fprintf(stderr,"binc: interp: buffer index %lld out of range\n",idx); exit(1); }
    c->bufs[pi*IBUFSZ+idx]=v;
}
static IV ibuf_get(ICtx *c, int pi, long long idx){
    if(idx<0||idx>=IBUFSZ){ fprintf(stderr,"binc: interp: buffer index %lld out of range\n",idx); exit(1); }
    return c->bufs[pi*IBUFSZ+idx];
}

static int is_builtin(const char *n);
static Flow istmt(ICtx *c, Stmt *s);
static IV iexpr(ICtx *c, Expr *e);

static Flow iblock(ICtx *c, Block *b){ Flow fl=FL_NONE; for(size_t i=0;i<b->n && fl==FL_NONE;i++) fl=istmt(c,&b->stmts[i]); return fl; }

static int param_idx(Function *fn, const char *name){ for(size_t i=0;i<fn->nparams;i++) if(!strcmp(fn->params[i].name,name)) return (int)i; return -1; }

/* one math builtin applied per lane; returns 1 if handled */
static int math_unary(const char *n, double x, double *out){
    if(!strcmp(n,"sqrt")) *out=sqrt(x);
    else if(!strcmp(n,"rsqrt")) *out=1.0/sqrt(x);
    else if(!strcmp(n,"sin")) *out=sin(x);
    else if(!strcmp(n,"cos")) *out=cos(x);
    else if(!strcmp(n,"tan")) *out=tan(x);
    else if(!strcmp(n,"asin")) *out=asin(x);
    else if(!strcmp(n,"acos")) *out=acos(x);
    else if(!strcmp(n,"atan")) *out=atan(x);
    else if(!strcmp(n,"exp")) *out=exp(x);
    else if(!strcmp(n,"log")) *out=log(x);
    else if(!strcmp(n,"exp2")) *out=exp2(x);
    else if(!strcmp(n,"log2")) *out=log2(x);
    else if(!strcmp(n,"abs")||!strcmp(n,"fabs")) *out=fabs(x);
    else if(!strcmp(n,"floor")) *out=floor(x);
    else if(!strcmp(n,"ceil")) *out=ceil(x);
    else if(!strcmp(n,"fract")) *out=x-floor(x);
    else if(!strcmp(n,"sign")) *out=x>0?1.0:(x<0?-1.0:0.0);
    else if(!strcmp(n,"radians")) *out=x*0.017453292519943295;
    else if(!strcmp(n,"degrees")) *out=x*57.29577951308232;
    else return 0;
    return 1;
}

static int struct_field(const Program *p, const char *tag, const char *field, Type *ft){
    for(size_t i=0;i<p->nstructs;i++) if(!strcmp(p->structs[i].tag,tag))
        for(size_t j=0;j<p->structs[i].nfields;j++)
            if(!strcmp(p->structs[i].fields[j].name,field)){ *ft=p->structs[i].fields[j].ty; return (int)j; }
    return -1;
}

static IV iexpr(ICtx *c, Expr *e){
    switch(e->kind){
    case E_FCONST: return iv_num(T_FLOAT,e->fval);
    case E_ICONST: return iv_i(T_INT32,e->ival);
    case E_BOOL: return iv_i(T_BOOL,e->bval);
    case E_IDENT:{
        int li=iloc_find(c,e->name);
        if(li>=0) return c->locs[li].v;
        int pi=param_idx(c->fn,e->name);
        if(pi>=0){ Param *p=&c->fn->params[pi];
            if(p->ty.kind==T_COORD) return iv_i(T_INT32,c->cur);
            if(p->ty.is_ptr||p->ty.array_n){ fprintf(stderr,"binc: interp: bare buffer name in expression\n"); exit(1); }
            return iv_zero(p->ty.kind,p->ty.vecn>1?p->ty.vecn:0); /* uniform scalar: 0 */
        }
        for(size_t i=0;i<c->prog->nconsts;i++) if(!strcmp(c->prog->consts[i].name,e->name)){
            const ConstDef *cd=&c->prog->consts[i];
            return cd->ty.kind==T_FLOAT?iv_num(T_FLOAT,cd->fval):iv_i(cd->ty.kind,cd->ival);
        }
        fprintf(stderr,"binc: interp: unknown identifier %s\n",e->name); exit(1);
    }
    case E_INDEX:{
        IV base=iexpr(c,e->operand);
        IV ix=iexpr(c,e->rhs); long long idx=ix.i[0];
        if(e->operand->kind==E_IDENT){
            int pi=param_idx(c->fn,e->operand->name);
            if(pi>=0 && c->fn->params[pi].ty.is_ptr) return ibuf_get(c,pi,idx);
            int li=iloc_find(c,e->operand->name);
            if(li>=0 && c->locs[li].arr) return c->locs[li].arr[idx];
        }
        /* vector component index */
        if(base.vecn>1){ IV v={0}; v.k=base.k; v.vecn=0; v.f[0]=base.f[idx]; v.i[0]=base.i[idx]; return v; }
        fprintf(stderr,"binc: interp: unsupported index expression\n"); exit(1);
    }
    case E_FIELD:{
        IV base=iexpr(c,e->operand);
        if(e->operand->kind==E_IDENT){
            int li=iloc_find(c,e->operand->name);
            if(li>=0 && base.k==T_STRUCT){
                /* struct local: field values packed in the loc's arr */
                int fi=struct_field(c->prog, (const char*)c->locs[li].v.i[3], e->field, NULL);
                /* tag stored in a parallel lookup: search all structs by the field name */
                (void)fi;
            }
        }
        /* vector swizzle/component */
        if(base.vecn>1){
            const char *sw=e->field;
            if(!strcmp(sw,"x")||!strcmp(sw,"r")) { IV v=base; v.vecn=0; return v; }
            if(!strcmp(sw,"y")||!strcmp(sw,"g")) { IV v=base; v.vecn=0; v.f[0]=base.f[1]; v.i[0]=base.i[1]; return v; }
            if(!strcmp(sw,"z")||!strcmp(sw,"b")) { IV v=base; v.vecn=0; v.f[0]=base.f[2]; v.i[0]=base.i[2]; return v; }
            if(!strcmp(sw,"w")||!strcmp(sw,"a")) { IV v=base; v.vecn=0; v.f[0]=base.f[3]; v.i[0]=base.i[3]; return v; }
            /* multi-component swizzle */
            int lanes[4], n=0; const char *q=sw;
            while(*q){ int ci=*q=='x'?0:*q=='r'?0:*q=='y'?1:*q=='g'?1:*q=='z'?2:*q=='b'?2:*q=='w'?3:3; lanes[n++]=ci; q++; }
            IV v=iv_zero(base.k,n); for(int i=0;i<n;i++){ v.f[i]=base.f[lanes[i]]; v.i[i]=base.i[lanes[i]]; }
            return v;
        }
        fprintf(stderr,"binc: interp: unsupported field access\n"); exit(1);
    }
    case E_DEREF:{
        if(e->operand->kind==E_IDENT){ int pi=param_idx(c->fn,e->operand->name); if(pi>=0) return ibuf_get(c,pi,0); }
        fprintf(stderr,"binc: interp: unsupported dereference\n"); exit(1);
    }
    case E_NEG:{ IV a=iexpr(c,e->operand); if(a.vecn){ IV v=a; for(int i=0;i<a.vecn;i++){v.f[i]=-a.f[i];v.i[i]=-a.i[i];} return v; }
        return is_float_k(a.k)?iv_num(T_FLOAT,-a.f[0]):iv_i(a.k,-a.i[0]); }
    case E_NOT:{ IV a=iexpr(c,e->operand); return iv_i(T_BOOL,!a.i[0]); }
    case E_COMPL:{ IV a=iexpr(c,e->operand); return iv_i(a.k,~(unsigned)a.i[0]); }
    case E_BIN:{
        IV a=iexpr(c,e->lhs), b=iexpr(c,e->rhs);
        int n=a.vecn>1?a.vecn:1; int flt=is_float_k(a.k)||is_float_k(b.k);
        TypeKind k=flt?T_FLOAT:(a.k==T_UINT32||b.k==T_UINT32?T_UINT32:T_INT32);
        IV v=iv_zero(k,a.vecn>1?a.vecn:0);
        for(int i=0;i<n;i++){
            double af=flt?iv_flt(a):0, bf=flt?iv_flt(b):0;
            long long ai=a.vecn? (is_float_k(a.k)?(long long)a.f[i]:a.i[i]) : (is_float_k(a.k)?(long long)a.f[0]:a.i[0]);
            long long bi=b.vecn? (is_float_k(b.k)?(long long)b.f[i]:b.i[i]) : (is_float_k(b.k)?(long long)b.f[0]:b.i[0]);
            switch(e->bop){
            case B_ADD: if(flt){v.f[i]=af+bf;} else v.i[i]=ai+bi; break;
            case B_SUB: if(flt){v.f[i]=af-bf;} else v.i[i]=ai-bi; break;
            case B_MUL: if(flt){v.f[i]=af*bf;} else v.i[i]=ai*bi; break;
            case B_DIV: if(flt){v.f[i]=af/bf;} else v.i[i]=ai/bi; break;
            case B_MOD: if(flt){v.f[i]=fmod(af,bf);} else v.i[i]=ai%bi; break;
            case B_AND: v.i[i]=ai&bi; break;
            case B_OR: v.i[i]=ai|bi; break;
            case B_XOR: v.i[i]=ai^bi; break;
            case B_SHL: v.i[i]=ai<<(bi&31); break;
            case B_SHR: v.i[i]=(long long)((unsigned)ai>>(bi&31)); break;
            }
        }
        return v;
    }
    case E_CMP:{
        IV a=iexpr(c,e->lhs), b=iexpr(c,e->rhs);
        int n=a.vecn>1?a.vecn:1; int flt=is_float_k(a.k)||is_float_k(b.k);
        IV v=iv_zero(T_BOOL,a.vecn>1?a.vecn:0);
        for(int i=0;i<n;i++){
            double af=flt?iv_flt(a):0, bf=flt?iv_flt(b):0;
            long long ai=a.vecn?(is_float_k(a.k)?(long long)a.f[i]:a.i[i]):(is_float_k(a.k)?(long long)a.f[0]:a.i[0]);
            long long bi=b.vecn?(is_float_k(b.k)?(long long)b.f[i]:b.i[i]):(is_float_k(b.k)?(long long)b.f[0]:b.i[0]);
            int r=0;
            switch(e->cmp){
            case C_EQ: r=flt?(af==bf):(ai==bi); break;
            case C_NE: r=flt?(af!=bf):(ai!=bi); break;
            case C_LT: r=flt?(af<bf):(ai<bi); break;
            case C_LE: r=flt?(af<=bf):(ai<=bi); break;
            case C_GT: r=flt?(af>bf):(ai>bi); break;
            case C_GE: r=flt?(af>=bf):(ai>=bi); break;
            }
            if(a.vecn>1){ v.f[i]=r; v.i[i]=r; } else { v.f[0]=r; v.i[0]=r; }
        }
        return v;
    }
    case E_LOG:{
        IV a=iexpr(c,e->lhs);
        if(e->log==L_AND && !a.i[0]) return iv_i(T_BOOL,0);
        if(e->log==L_OR && a.i[0]) return iv_i(T_BOOL,1);
        IV b=iexpr(c,e->rhs); return iv_i(T_BOOL,b.i[0]!=0);
    }
    case E_TERNARY:{
        IV cv=iexpr(c,e->operand);
        return cv.i[0]?iexpr(c,e->lhs):iexpr(c,e->rhs);
    }
    case E_INCDEC:{
        IV a=iexpr(c,e->operand);
        IV nv=a; if(e->bval){ nv.f[0]-=1; nv.i[0]-=1; } else { nv.f[0]+=1; nv.i[0]+=1; }
        if(e->operand->kind==E_IDENT) iloc_set(c,e->operand->name,nv);
        return a;
    }
    case E_CAST:{
        IV a=iexpr(c,e->operand); Type t=e->cty;
        int n=a.vecn>1?a.vecn:1;
        IV v=iv_zero(t.kind,t.vecn>1?t.vecn:0);
        for(int i=0;i<n;i++){
            double f=a.vecn?a.f[i]:a.f[0]; long long x=a.vecn?a.i[i]:a.i[0];
            switch(t.kind){
            case T_FLOAT: case T_HALF: v.f[i]=a.k==T_INT32||a.k==T_UINT32?(double)x:f; break;
            case T_INT32: case T_UINT32: v.i[i]=is_float_k(a.k)?(long long)f:(long long)(int)(unsigned)x; break;
            case T_BOOL: v.i[i]=is_float_k(a.k)?(f!=0.0):(x!=0); break;
            default: break;
            }
        }
        return v;
    }
    case E_ASSIGN:{
        if(e->aop!=A_ASSIGN){
            /* compound: lvalue = lvalue OP rhs */
            IV cur=iexpr(c,e->operand);
            Expr rhs={0}; rhs.kind=E_BIN; rhs.bop=(BinOp)(e->aop-1); rhs.lhs=e->operand; rhs.rhs=e->rhs;
            /* rebuild by evaluating the binary op on a copy of the lvalue */
            switch(e->aop){
            case A_ADDEQ: { IV r=iexpr(c,e->rhs); if(is_float_k(cur.k)||is_float_k(r.k)){cur.f[0]+=iv_flt(r);cur.i[0]=(long long)cur.f[0];} else cur.i[0]+=r.i[0]; break; }
            case A_SUBEQ: { IV r=iexpr(c,e->rhs); if(is_float_k(cur.k)||is_float_k(r.k)){cur.f[0]-=iv_flt(r);cur.i[0]=(long long)cur.f[0];} else cur.i[0]-=r.i[0]; break; }
            case A_MULEQ: { IV r=iexpr(c,e->rhs); if(is_float_k(cur.k)||is_float_k(r.k)){cur.f[0]*=iv_flt(r);cur.i[0]=(long long)cur.f[0];} else cur.i[0]*=r.i[0]; break; }
            case A_DIVEQ: { IV r=iexpr(c,e->rhs); if(is_float_k(cur.k)||is_float_k(r.k)){cur.f[0]/=iv_flt(r);cur.i[0]=(long long)cur.f[0];} else cur.i[0]/=r.i[0]; break; }
            case A_MODEQ: { IV r=iexpr(c,e->rhs); cur.i[0]%=r.i[0]; break; }
            case A_ANDEQ: { IV r=iexpr(c,e->rhs); cur.i[0]&=r.i[0]; break; }
            case A_OREQ: { IV r=iexpr(c,e->rhs); cur.i[0]|=r.i[0]; break; }
            case A_XOREQ: { IV r=iexpr(c,e->rhs); cur.i[0]^=r.i[0]; break; }
            case A_SHLEQ: { IV r=iexpr(c,e->rhs); cur.i[0]<<=(r.i[0]&31); break; }
            case A_SHREQ: { IV r=iexpr(c,e->rhs); cur.i[0]=(long long)((unsigned)cur.i[0]>>(r.i[0]&31)); break; }
            default: break;
            }
            if(e->operand->kind==E_IDENT) iloc_set(c,e->operand->name,cur);
            return cur;
        }
        IV rv=iexpr(c,e->rhs);
        if(e->operand->kind==E_IDENT){ iloc_set(c,e->operand->name,rv); return rv; }
        if(e->operand->kind==E_INDEX){
            IV ix=iexpr(c,e->operand->rhs); long long idx=ix.i[0];
            if(e->operand->operand->kind==E_IDENT){
                int pi=param_idx(c->fn,e->operand->operand->name);
                if(pi>=0 && c->fn->params[pi].ty.is_ptr){ ibuf_set(c,pi,idx,rv); return rv; }
                int li=iloc_find(c,e->operand->operand->name);
                if(li>=0 && c->locs[li].arr){ c->locs[li].arr[idx]=rv; return rv; }
            }
            /* vector component assignment v[i] = x */
            if(e->operand->operand->kind==E_IDENT){
                IV base=iexpr(c,e->operand->operand);
                if(base.vecn>1){ base.f[idx]=rv.f[0]; base.i[idx]=rv.i[0];
                    iloc_set(c,e->operand->operand->name,base); return rv; }
            }
        }
        if(e->operand->kind==E_FIELD && e->operand->operand->kind==E_IDENT){
            const char *vname=e->operand->operand->name;
            IV base=iexpr(c,e->operand->operand);
            if(base.vecn>1){ /* swizzle assignment */
                const char *q=e->operand->field; int li=0;
                while(*q){ int ci=*q=='x'||*q=='r'?0:*q=='y'||*q=='g'?1:*q=='z'||*q=='b'?2:3;
                    base.f[ci]=rv.vecn?rv.f[li]:rv.f[0]; base.i[ci]=rv.vecn?rv.i[li]:rv.i[0]; li++; q++; }
                iloc_set(c,vname,base); return rv;
            }
        }
        fprintf(stderr,"binc: interp: unsupported assignment target\n"); exit(1);
    }
    case E_CALL:{
        if(e->callee){ fprintf(stderr,"binc: interp: methods are not supported\n"); exit(1); }
        /* vector constructor / builtin / user function */
        TypeKind cb; int cn;
        if(!strcmp(e->name,"float2")||!strcmp(e->name,"float3")||!strcmp(e->name,"float4")) cb=T_FLOAT;
        else if(!strcmp(e->name,"int2")||!strcmp(e->name,"int3")||!strcmp(e->name,"int4")) cb=T_INT32;
        else if(!strcmp(e->name,"uint2")||!strcmp(e->name,"uint3")||!strcmp(e->name,"uint4")) cb=T_UINT32;
        else cb=T_VOID;
        cn = e->name[strlen(e->name)-1]-'0';
        if(cb!=T_VOID){
            IV v=iv_zero(cb,cn); int li=0;
            for(size_t a=0;a<e->nargs;a++){ IV av=iexpr(c,e->args[a]);
                if(av.vecn>1){ for(int j=0;j<av.vecn;j++){ v.f[li]=av.f[j]; v.i[li]=av.i[j]; li++; } }
                else { v.f[li]=av.f[0]; v.i[li]=av.i[0]; li++; } }
            return v;
        }
        if(is_builtin(e->name)){
            /* unary scalar/vector math */
            IV a=iexpr(c,e->args[0]);
            int n=a.vecn>1?a.vecn:1;
            IV v=iv_zero(T_FLOAT,a.vecn>1?a.vecn:0);
            for(int i=0;i<n;i++){
                double x=a.vecn?a.f[i]:a.f[0]; double r=0;
                if(!math_unary(e->name,x,&r)){ /* binary forms and composites */
                    if(e->nargs>=2){
                        IV b=iexpr(c,e->args[1]);
                        double y=b.vecn?b.f[i]:b.f[0];
                    if(!strcmp(e->name,"atan2")) r=atan2(x,y);
                    else if(!strcmp(e->name,"pow")) r=pow(x,y);
                    else if(!strcmp(e->name,"fmin")||!strcmp(e->name,"min")) r=fmin(x,y);
                    else if(!strcmp(e->name,"fmax")||!strcmp(e->name,"max")) r=fmax(x,y);
                    else if(!strcmp(e->name,"step")) r=x<y?0.0:1.0; /* step(edge,x) */
                    else if(!strcmp(e->name,"mod")) r=fmod(x,y);
                    else if(!strcmp(e->name,"mix")){ IV t=iexpr(c,e->args[2]); double tv=t.vecn?t.f[i]:t.f[0]; r=x+(y-x)*tv; }
                    else if(!strcmp(e->name,"clamp")){ IV lo=iexpr(c,e->args[1]), hi=iexpr(c,e->args[2]); r=fmin(fmax(x,lo.vecn?lo.f[i]:lo.f[0]),hi.vecn?hi.f[i]:hi.f[0]); y=0; }
                    else if(!strcmp(e->name,"smoothstep")){ IV e0=iexpr(c,e->args[1]), e1=iexpr(c,e->args[2]); double t=fmin(fmax((x-e0.f[i])/(e1.f[i]-e0.f[i]),0.0),1.0); r=t*t*(3-2*t); }
                    else if(!strcmp(e->name,"length")) r=sqrt(x*x+y*y);
                    else if(!strcmp(e->name,"distance")){ /* vector handled below */ r=0; }
                    else if(!strcmp(e->name,"dot")) r=x*y;
                    else if(!strcmp(e->name,"imin")) r=fmin(x,y);
                    else if(!strcmp(e->name,"imax")) r=fmax(x,y);
                    else { fprintf(stderr,"binc: interp: unsupported builtin %s\n",e->name); exit(1); }
                    }
                }
                v.f[i]=r; v.i[i]=(long long)r;
            }
            /* vector-only composites */
            if(a.vecn>1 && (!strcmp(e->name,"length")||!strcmp(e->name,"normalize")||!strcmp(e->name,"dot")||
                            !strcmp(e->name,"distance")||!strcmp(e->name,"cross")||!strcmp(e->name,"reflect"))){
                if(!strcmp(e->name,"length")){ double s=0; for(int i=0;i<a.vecn;i++) s+=a.f[i]*a.f[i]; v=iv_num(T_FLOAT,sqrt(s)); }
                else if(!strcmp(e->name,"normalize")){ double s=0; for(int i=0;i<a.vecn;i++) s+=a.f[i]*a.f[i]; s=sqrt(s);
                    for(int i=0;i<a.vecn;i++){ v.f[i]=a.f[i]/s; v.i[i]=(long long)(a.f[i]/s); } }
                else if(!strcmp(e->name,"dot")){ IV b=iexpr(c,e->args[1]); double s=0; for(int i=0;i<a.vecn;i++) s+=a.f[i]*b.f[i]; v=iv_num(T_FLOAT,s); }
                else if(!strcmp(e->name,"distance")){ IV b=iexpr(c,e->args[1]); double s=0; for(int i=0;i<a.vecn;i++){double d=a.f[i]-b.f[i]; s+=d*d;} v=iv_num(T_FLOAT,sqrt(s)); }
                else if(!strcmp(e->name,"cross")){ IV b=iexpr(c,e->args[1]); v=iv_zero(T_FLOAT,3);
                    v.f[0]=a.f[1]*b.f[2]-a.f[2]*b.f[1]; v.f[1]=a.f[2]*b.f[0]-a.f[0]*b.f[2]; v.f[2]=a.f[0]*b.f[1]-a.f[1]*b.f[0]; }
                else if(!strcmp(e->name,"reflect")){ IV b=iexpr(c,e->args[1]); double d=0; for(int i=0;i<a.vecn;i++) d+=a.f[i]*b.f[i];
                    for(int i=0;i<a.vecn;i++) v.f[i]=a.f[i]-2*d*b.f[i]; }
            }
            return v;
        }
        /* user function */
        Function *f=NULL; for(size_t i=0;i<c->prog->nfuncs;i++) if(!strcmp(c->prog->funcs[i].name,e->name)){ f=&c->prog->funcs[i]; break; }
        if(!f){ fprintf(stderr,"binc: interp: undefined function %s\n",e->name); exit(1); }
        if(c->depth>=MAX_DEPTH){ fprintf(stderr,"binc: interp: recursion/depth limit\n"); exit(1); }
        /* bind args to fresh locals */
        ILoc *save_locs=c->locs; int save_n=c->nlocs, save_cap=c->lcap; Function *save_fn=c->fn; int save_depth=c->depth;
        c->locs=NULL; c->nlocs=0; c->lcap=0; c->fn=f; c->depth++;
        for(size_t a=0;a<f->nparams;a++){
            Param *p=&f->params[a]; IV av=iexpr(c,e->args[a]);
            if(p->ty.is_ptr){ int pi=param_idx(save_fn,p->name); (void)pi; iloc_set(c,p->name,av); } /* pointer pass-through not modeled */
            else iloc_set(c,p->name,av);
        }
        Flow fl=iblock(c,&f->body); (void)fl;
        IV ret=c->has_ret?c->ret:iv_zero(f->ret.kind,f->ret.vecn>1?f->ret.vecn:0);
        free(c->locs); c->locs=save_locs; c->nlocs=save_n; c->lcap=save_cap; c->fn=save_fn; c->depth=save_depth;
        return ret;
    }
    }
    fprintf(stderr,"binc: interp: unhandled expression\n"); exit(1);
}

static int is_builtin(const char *n){
    static const char *names[] = { "sqrt","rsqrt","sin","cos","tan","asin","acos","atan","atan2","pow",
        "exp","log","exp2","log2","abs","fabs","fmin","fmax","imin","imax","floor","ceil","fract","mod",
        "sign","radians","degrees","mix","clamp","step","smoothstep","dot","cross","length","distance",
        "normalize","reflect","select", NULL };
    for(int i=0;names[i];i++) if(!strcmp(names[i],n)) return 1;
    return 0;
}

static Flow istmt(ICtx *c, Stmt *s){
    switch(s->kind){
    case S_EXPR: iexpr(c,s->expr); return FL_NONE;
    case S_DECL:{
        Type t=s->ty;
        if(t.array_n){ iloc_arr(c,s->name,t.array_n); return FL_NONE; }
        if(t.kind==T_STRUCT){ fprintf(stderr,"binc: interp: struct locals not supported\n"); exit(1); }
        IV v = s->init? iexpr(c,s->init) : iv_zero(t.kind,t.vecn>1?t.vecn:0);
        iloc_set(c,s->name,v); return FL_NONE;
    }
    case S_RETURN: if(s->expr){ c->ret=iexpr(c,s->expr); c->has_ret=1; } return FL_RET;
    case S_IF:{
        IV cv=iexpr(c,s->cond);
        if(cv.i[0]) return iblock(c,&s->then_b);
        return iblock(c,&s->else_b);
    }
    case S_WHILE:{
        Flow fl=FL_NONE;
        while(fl==FL_NONE){ IV cv=iexpr(c,s->cond); if(!cv.i[0]) break; fl=iblock(c,&s->then_b); if(fl==FL_BREAK)fl=FL_NONE; if(fl==FL_CONT)fl=FL_NONE; }
        return fl;
    }
    case S_DOWHILE:{
        Flow fl=FL_NONE;
        do{ fl=iblock(c,&s->then_b); if(fl==FL_BREAK)fl=FL_NONE; if(fl==FL_CONT)fl=FL_NONE;
            if(fl!=FL_NONE) break; IV cv=iexpr(c,s->cond); if(!cv.i[0]) break; } while(1);
        return fl;
    }
    case S_FOR:{
        if(s->for_init){ if(s->for_init->kind==S_DECL) istmt(c,s->for_init); else iexpr(c,s->for_init->expr); }
        Flow fl=FL_NONE;
        for(;;){
            if(s->for_cond){ IV cv=iexpr(c,s->for_cond); if(!cv.i[0]) break; }
            fl=iblock(c,&s->then_b);
            if(fl==FL_BREAK){ fl=FL_NONE; break; }
            if(fl==FL_CONT) fl=FL_NONE;
            if(fl!=FL_NONE) break;
            if(s->for_incr) iexpr(c,s->for_incr);
        }
        return fl;
    }
    case S_SWITCH:{
        IV sv=iexpr(c,s->sw_cond); long long key=sv.i[0];
        /* find the matching case or default */
        size_t start=s->ncases;
        for(size_t i=0;i<s->ncases;i++){ IV cv=iexpr(c,s->cases[i].val); if(cv.i[0]==key){ start=i; break; } }
        if(start==s->ncases && !s->has_default) return FL_NONE;
        Flow fl=FL_NONE;
        for(size_t i=start;i<s->ncases && fl==FL_NONE;i++) fl=iblock(c,&s->cases[i].body);
        if(fl==FL_NONE && start==s->ncases) fl=iblock(c,&s->def_body);
        return fl;
    }
    case S_BLOCK: return iblock(c,&s->then_b);
    case S_BREAK: return FL_BREAK;
    case S_CONTINUE: return FL_CONT;
    }
    return FL_NONE;
}

void interp_run(const Program *prog){
    ICtx c={0}; c.prog=prog; c.grid=4;
    /* device buffers: one 256-word array per buffer parameter of each kernel */
    int nbufs=0; for(size_t i=0;i<prog->nfuncs;i++) for(size_t p=0;p<prog->funcs[i].nparams;p++)
        if(prog->funcs[i].params[p].ty.is_ptr) nbufs=prog->funcs[i].nparams;
    c.bufs=calloc((size_t)(nbufs?nbufs:1)*IBUFSZ,sizeof(IV));
    c.nbufs=nbufs;
    for(size_t fi=0;fi<prog->nfuncs;fi++){
        Function *fn=&prog->funcs[fi];
        if(!fn->is_kernel) continue;
        c.fn=fn;
        for(int t=0;t<c.grid;t++){
            c.cur=t; c.nlocs=0; c.has_ret=0; c.depth=0;
            Flow fl=iblock(&c,&fn->body); (void)fl;
        }
        /* print buffer 0 */
        TypeKind bt=T_FLOAT; int pi=param_idx(fn,"out");
        if(pi<0) for(size_t i=0;i<fn->nparams;i++) if(fn->params[i].ty.is_ptr){ pi=(int)i; break; }
        if(pi>=0) bt=fn->params[pi].ty.kind;
        printf("buf0:");
        for(int i=0;i<16;i++){
            IV v=c.bufs[(size_t)pi*IBUFSZ+i];
            if(is_float_k(bt)) printf(" %.6f",v.f[0]); else printf(" %lld",(long long)(v.i[0]&0xFFFFFFFF));
        }
        printf("\n");
        break; /* only the first kernel prints */
    }
    free(c.bufs);
}
