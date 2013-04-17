/* Scan the bench.tokudb/bench.db over and over. */
#define DONT_DEPRECATE_MALLOC

#include <toku_portability.h>
#include <assert.h>
#include <db.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef TOKUDB
#include "key.h"
#include "cachetable.h"
#include "trace_mem.h"
#endif

const char *pname;
enum run_mode { RUN_HWC, RUN_LWC, RUN_VERIFY, RUN_HEAVI } run_mode = RUN_HWC;
int do_txns=1, prelock=0, prelockflag=0;
u_int32_t lock_flag = 0;
long limitcount=-1;
u_int32_t cachesize = 127*1024*1024;
static int do_mysql = 0;

static void parse_args (int argc, const char *argv[]) {
    pname=argv[0];
    argc--;
    argv++;
    int specified_run_mode=0;
    while (argc>0) {
	if (strcmp(*argv,"--verify-lwc")==0) {
	    if (specified_run_mode && run_mode!=RUN_VERIFY) { two_modes: fprintf(stderr, "You specified two run modes\n"); exit(1); }
	    run_mode = RUN_VERIFY;
	} else if (strcmp(*argv, "--lwc")==0)  {
	    if (specified_run_mode && run_mode!=RUN_LWC) goto two_modes;
	    run_mode = RUN_LWC;
	} else if (strcmp(*argv, "--hwc")==0)  {
	    if (specified_run_mode && run_mode!=RUN_VERIFY) goto two_modes;
	    run_mode = RUN_HWC;
	} else if (strcmp(*argv, "--prelock")==0) prelock=1;
#ifdef TOKUDB
	else if (strcmp(*argv, "--heavi")==0)  {
	    if (specified_run_mode && run_mode!=RUN_HEAVI) goto two_modes;
	    run_mode = RUN_HEAVI;
        }
        else if (strcmp(*argv, "--prelockflag")==0)      { prelockflag=1; lock_flag = DB_PRELOCKED; }
        else if (strcmp(*argv, "--prelockwriteflag")==0) { prelockflag=1; lock_flag = DB_PRELOCKED_WRITE; }
#endif
	else if (strcmp(*argv, "--nox")==0)              { do_txns=0; }
	else if (strcmp(*argv, "--count")==0)            {
	    char *end;
	    argv++; argc--;
	    errno=0; limitcount=strtol(*argv, &end, 10); assert(errno==0);
	    printf("Limiting count to %ld\n", limitcount);
        } else if (strcmp(*argv, "--cachesize")==0 && argc>0) {
            char *end;
            argv++; argc--;
            cachesize=(u_int32_t)strtol(*argv, &end, 10);
        } else if (strcmp(*argv, "--mysql") == 0) {
            do_mysql = 1;
	} else {
	    fprintf(stderr, "Usage:\n%s [--verify-lwc | --lwc | --nohwc] [--prelock] [--prelockflag] [--prelockwriteflag]\n", pname);
	    fprintf(stderr, "  --hwc               run heavy weight cursors (this is the default)\n");
	    fprintf(stderr, "  --verify-lwc        means to run the light weight cursor and the heavyweight cursor to verify that they get the same answer.\n");
	    fprintf(stderr, "  --lwc               run light weight cursors instead of heavy weight cursors\n");
	    fprintf(stderr, "  --prelock           acquire a read lock on the entire table before running\n");
	    fprintf(stderr, "  --prelockflag       pass DB_PRELOCKED to the the cursor get operation whenever the locks have been acquired\n");
	    fprintf(stderr, "  --prelockwriteflag  pass DB_PRELOCKED_WRITE to the cursor get operation\n");
	    fprintf(stderr, "  --nox               no transactions\n");
	    fprintf(stderr, "  --count <count>     read the first COUNT rows and then  stop.\n");
            fprintf(stderr, "  --cachesize <n>     set the env cachesize to <n>\n");
            fprintf(stderr, "  --mysql             compare keys that are mysql big int not null types\n");
            exit(1);
	}
	argc--;
	argv++;
    }
}


DB_ENV *env;
DB *db;
DB_TXN *tid=0;

#define STRINGIFY2(s) #s
#define STRINGIFY(s) STRINGIFY2(s)
const char *dbdir = "./bench."  STRINGIFY(DIRSUF); /* DIRSUF is passed in as a -D argument to the compiler. */
int env_open_flags_yesx = DB_CREATE|DB_PRIVATE|DB_INIT_MPOOL|DB_INIT_TXN|DB_INIT_LOG|DB_INIT_LOCK;
int env_open_flags_nox = DB_CREATE|DB_PRIVATE|DB_INIT_MPOOL;
char *dbfilename = "bench.db";

static inline uint64_t mysql_get_bigint(unsigned char *d) {
    uint64_t r = 0;
    memcpy(&r, d, sizeof r);
    return r;
}

static int mysql_key_compare(DB *mydb __attribute__((unused)),
                               const DBT *adbt, const DBT *bdbt) {
    unsigned char *adata = adbt->data;
    unsigned char *bdata = bdbt->data;
    uint64_t a, b;
    assert(adbt->size == 9 && bdbt->size == 9);
    assert(adata[0] == 0 && bdata[0] == 0);
    a = mysql_get_bigint(adata+1);
    b = mysql_get_bigint(bdata+1);
    if (a < b) return -1;
    if (a > b) return +1;
    return 0;
}

static void scanscan_setup (void) {
    int r;
    r = db_env_create(&env, 0);                                                           assert(r==0);
    r = env->set_cachesize(env, 0, cachesize, 1);                                         assert(r==0);
    r = env->open(env, dbdir, do_txns? env_open_flags_yesx : env_open_flags_nox, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);   assert(r==0);
    r = db_create(&db, env, 0);                                                           assert(r==0);
    if (do_mysql) {
        r = db->set_bt_compare(db, mysql_key_compare); assert(r == 0);
    }
    if (do_txns) {
	r = env->txn_begin(env, 0, &tid, 0);                                              assert(r==0);
    }
    r = db->open(db, tid, dbfilename, NULL, DB_BTREE, 0, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);                           assert(r==0);
#ifdef TOKUDB
    if (prelock) {
	r = db->pre_acquire_read_lock(db,
				      tid,
				      db->dbt_neg_infty(), db->dbt_neg_infty(),
				      db->dbt_pos_infty(), db->dbt_pos_infty());
	assert(r==0);
    }
#endif
}

static void scanscan_shutdown (void) {
    int r;
    r = db->close(db, 0);                                       assert(r==0);
    if (do_txns) {
	r = tid->commit(tid, 0);                                    assert(r==0);
    }
    r = env->close(env, 0);                                     assert(r==0);

#if 0 && defined TOKUDB
    {
	extern int toku_os_get_max_rss(int64_t*);
        int64_t mrss;
        int r = toku_os_get_max_rss(&mrss);
        assert(r==0);
	printf("maxrss=%.2fMB\n", mrss/256.0);
    }
#endif
}

static double gettime (void) {
    struct timeval tv;
    int r = gettimeofday(&tv, 0);
    assert(r==0);
    return tv.tv_sec + 1e-6*tv.tv_usec;
}

static void scanscan_hwc (void) {
    int r;
    int counter=0;
    for (counter=0; counter<2; counter++) {
	long long totalbytes=0;
	int rowcounter=0;
	double prevtime = gettime();
	DBT k,v;
	DBC *dbc;
	r = db->cursor(db, tid, &dbc, 0);                           assert(r==0);
	memset(&k, 0, sizeof(k));
	memset(&v, 0, sizeof(v));
        u_int32_t c_get_flags = DB_NEXT;
        if (prelockflag && (counter || prelock)) {
            c_get_flags |= lock_flag;
        }
	while (0 == (r = dbc->c_get(dbc, &k, &v, c_get_flags))) {
	    totalbytes += k.size + v.size;
	    rowcounter++;
	    if (limitcount>0 && rowcounter>=limitcount) break;
	}
	r = dbc->c_close(dbc);                                      assert(r==0);
	double thistime = gettime();
	double tdiff = thistime-prevtime;
	printf("Scan    %lld bytes (%d rows) in %9.6fs at %9fMB/s\n", totalbytes, rowcounter, tdiff, 1e-6*totalbytes/tdiff);
    }
}

#ifdef TOKUDB

struct extra_count {
    long long totalbytes;
    int rowcounter;
};

static int counttotalbytes (DBT const *key, DBT const *data, void *extrav) {
    struct extra_count *e=extrav;
    e->totalbytes += key->size + data->size;
    e->rowcounter++;
    if (do_mysql && 0) {
        static uint64_t expect_key = 0;
        uint64_t k = mysql_get_bigint((unsigned char*)key->data+1);
        if (k != expect_key)
            printf("%s:%d %"PRIu64" %"PRIu64"\n", __FUNCTION__, __LINE__, k, expect_key);
        expect_key = k + 1;
    }
    return 0;
}

static void scanscan_lwc (void) {
    int r;
    int counter=0;
    for (counter=0; counter<2; counter++) {
	struct extra_count e = {0,0};
	double prevtime = gettime();
	DBC *dbc;
	r = db->cursor(db, tid, &dbc, 0);                           assert(r==0);
        u_int32_t f_flags = 0;
        if (prelockflag && (counter || prelock)) {
            f_flags |= lock_flag;
        }
	long rowcounter=0;
	while (0 == (r = dbc->c_getf_next(dbc, f_flags, counttotalbytes, &e))) {
	    rowcounter++;
	    if (limitcount>0 && rowcounter>=limitcount) break;
	}
	r = dbc->c_close(dbc);                                      assert(r==0);
	double thistime = gettime();
	double tdiff = thistime-prevtime;
	printf("LWC Scan %lld bytes (%d rows) in %9.6fs at %9fMB/s\n", e.totalbytes, e.rowcounter, tdiff, 1e-6*e.totalbytes/tdiff);
    }
}

struct extra_heavi {
    long long totalbytes;
    int rowcounter;
    DBT key;
    DBT val;
};

static int
copy_dbt(DBT *target, DBT const *source) {
    int r;
    if (target->ulen < source->size) {
        target->data = realloc(target->data, source->size);
        target->ulen = source->size;
    }
    if (!target->data) r = ENOMEM;
    else {
        target->size = source->size;
        memcpy(target->data, source->data, target->size);
        r = 0;
    }
    return r;
}

typedef struct foo{int a; } FOO;

static int
heaviside_next(const DBT *key, const DBT *val, void *extra_h) {
    struct extra_heavi *e=extra_h;

    int cmp;
    cmp = toku_default_compare_fun(db, key, &e->key);
    if (cmp != 0) return cmp;
    if (val) cmp = toku_default_compare_fun(db, val, &e->val);
    if (cmp != 0) return cmp;
    return -1; //Return negative on <=, positive on >
}

static int copy_and_counttotalbytes (DBT const *key, DBT const *val, void *extrav, int r_h) {
    assert(r_h>0);
    struct extra_heavi *e=extrav;
    e->totalbytes += key->size + val->size;
    e->rowcounter++;
    int r;
    r = copy_dbt(&e->key, key);
    if (r==0) r = copy_dbt(&e->val, val);
    return r;
}

static void scanscan_heaviside (void) {
    int r;
    int counter=0;
    for (counter=0; counter<2; counter++) {
	struct extra_heavi e;
        memset(&e, 0, sizeof(e));
        e.key.flags = DB_DBT_REALLOC;
        e.val.flags = DB_DBT_REALLOC;
	double prevtime = gettime();
	DBC *dbc;
	r = db->cursor(db, tid, &dbc, 0);                           assert(r==0);
        u_int32_t f_flags = 0;
        if (prelockflag && (counter || prelock)) {
            f_flags |= lock_flag;
        }
        //Get first manually.
	long rowcounter=1;
        r = dbc->c_get(dbc, &e.key, &e.val, DB_FIRST | f_flags); assert(r==0);
        e.rowcounter = 1;
        e.totalbytes = e.key.size + e.val.size;

	while (0 == (r = dbc->c_getf_heaviside(dbc, f_flags, 
                        copy_and_counttotalbytes, &e,
                        heaviside_next, &e,
                        1))) {
	    rowcounter++;
	    if (limitcount>0 && rowcounter>=limitcount) break;
	}
        assert(rowcounter==e.rowcounter);
        if (e.key.data) {
            free(e.key.data);
            e.key.data = NULL;
        }
        if (e.val.data) {
            free(e.val.data);
            e.val.data = NULL;
        }
	r = dbc->c_close(dbc);                                      assert(r==0);
	double thistime = gettime();
	double tdiff = thistime-prevtime;
	printf("LWC Scan %lld bytes (%d rows) in %9.6fs at %9fMB/s\n", e.totalbytes, e.rowcounter, tdiff, 1e-6*e.totalbytes/tdiff);
    }
}

struct extra_verify {
    long long totalbytes;
    int rowcounter;
    DBT k,v; // the k and v are gotten using the old cursor
};

static int
checkbytes (DBT const *key, DBT const *data, void *extrav) {
    struct extra_verify *e=extrav;
    e->totalbytes += key->size + data->size;
    e->rowcounter++;
    assert(e->k.size == key->size);
    assert(e->v.size == data->size);
    assert(memcmp(e->k.data, key->data,  key->size)==0);
    assert(memcmp(e->v.data, data->data, data->size)==0);
    assert(e->k.data != key->data);
    assert(e->v.data != data->data);
    return 0;
}
    

static void scanscan_verify (void) {
    int r;
    int counter=0;
    for (counter=0; counter<2; counter++) {
	struct extra_verify v;
	v.totalbytes=0;
	v.rowcounter=0;
	double prevtime = gettime();
	DBC *dbc1, *dbc2;
	r = db->cursor(db, tid, &dbc1, 0);                           assert(r==0);
	r = db->cursor(db, tid, &dbc2, 0);                           assert(r==0);
	memset(&v.k, 0, sizeof(v.k));
	memset(&v.v, 0, sizeof(v.v));
        u_int32_t f_flags = 0;
        u_int32_t c_get_flags = DB_NEXT;
        if (prelockflag && (counter || prelock)) {
            f_flags     |= lock_flag;
            c_get_flags |= lock_flag;
        }
	while (1) {
	    int r1,r2;
	    r2 = dbc1->c_get(dbc1, &v.k, &v.v, c_get_flags);
	    r1 = dbc2->c_getf_next(dbc2, f_flags, checkbytes, &v);
	    assert(r1==r2);
	    if (r1) break;
	}
	r = dbc1->c_close(dbc1);                                      assert(r==0);
	r = dbc2->c_close(dbc2);                                      assert(r==0);
	double thistime = gettime();
	double tdiff = thistime-prevtime;
	printf("verify   %lld bytes (%d rows) in %9.6fs at %9fMB/s\n", v.totalbytes, v.rowcounter, tdiff, 1e-6*v.totalbytes/tdiff);
    }
}

#endif

int main (int argc, const char *argv[]) {

    parse_args(argc,argv);

    scanscan_setup();
    switch (run_mode) {
    case RUN_HWC:    scanscan_hwc();    break;
#ifdef TOKUDB
    case RUN_LWC:    scanscan_lwc();    break;
    case RUN_VERIFY: scanscan_verify(); break;
    case RUN_HEAVI:  scanscan_heaviside(); break;
#else
    default:         assert(0);         break;
#endif
    }
    scanscan_shutdown();

#if defined(TOKUDB)
    if (1) {
	toku_cachetable_print_hash_histogram();
    }

    // if tokudb has tracing enabled (see trace_mem.h) then this will dump
    // the trace data
    if (0) {
        toku_print_trace_mem();
    }
#endif
#if defined(__linux__) && __linux__
    char fname[256];
    sprintf(fname, "/proc/%d/status", toku_os_getpid());
    FILE *f = fopen(fname, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            int n;
            if (sscanf(line, "VmPeak: %d", &n) || sscanf(line, "VmHWM: %d", &n) || sscanf(line, "VmRSS: %d", &n))
                fputs(line, stdout);
        }
        fclose(f);
    }
#endif
    return 0;
}
