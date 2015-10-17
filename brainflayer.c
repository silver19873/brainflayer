/* Copyright (c) 2015 Ryan Castellucci, All Rights Reserved */
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include <openssl/sha.h>
#include <openssl/ripemd.h>
#include <openssl/obj_mac.h>

#include <arpa/inet.h> /* for ntohl/htonl */

#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/sysinfo.h>

#include "secp256k1/include/secp256k1.h"

#include "ec_pubkey_fast.h"

#include "hex.h"
#include "bloom.h"
#include "mmapf.h"
#include "hash160.h"

#include "brainv2.h"
#include "warpwallet.h"
#include "brainwalletio.h"

static int brainflayer_is_init = 0;

static unsigned char hash256[SHA256_DIGEST_LENGTH];
static unsigned char priv256[SHA256_DIGEST_LENGTH];
static hash160_t hash160_tmp;
static hash160_t hash160_compr;
static hash160_t hash160_uncmp;
static unsigned char *mem;

static mmapf_ctx bloom_mmapf;
static unsigned char *bloom = NULL;

static unsigned char hexed[4096], unhexed[4096];

static SHA256_CTX    *sha256_ctx;
static RIPEMD160_CTX *ripemd160_ctx;

static secp256k1_context_t *secp256k1_ctx;
static secp256k1_pubkey_t *pubkey;

#define bail(code, ...) \
do { \
  fprintf(stderr, __VA_ARGS__); \
  exit(code); \
} while (0)

uint64_t getns() {
  uint64_t ns;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ns  = ts.tv_nsec;
  ns += ts.tv_sec * 1000000000ULL;
  return ns;
}

static inline void brainflayer_init_globals() {
  /* only initialize stuff once */
  if (!brainflayer_is_init) {
    /* initialize buffers */
    mem = malloc(4096);

    /* initialize hashs */
    sha256_ctx    = malloc(sizeof(*sha256_ctx));
    ripemd160_ctx = malloc(sizeof(*ripemd160_ctx));
    /* initialize pubkey struct */
    pubkey = malloc(sizeof(*pubkey));

    /* set the flag */
    brainflayer_is_init = 1;
  }
}

inline static int priv2hash160(unsigned char *priv) {
  //brainflayer_init_globals();

  unsigned char *pub_chr = mem;
  int pub_chr_sz;

  secp256k1_ec_pubkey_create_precomp(pub_chr, &pub_chr_sz, priv);

#if 0
  int i = 0;
  for (i = 0; i < pub_chr_sz; i++) {
    printf("%02x", pub_chr[i]);
  }
  printf("\n");
#endif

  /* compute hash160 for uncompressed public key */
  /* sha256(pub) */
  SHA256_Init(sha256_ctx);
  SHA256_Update(sha256_ctx, pub_chr, 65);
  SHA256_Final(hash256, sha256_ctx);
  /* ripemd160(sha256(pub)) */
  RIPEMD160_Init(ripemd160_ctx);
  RIPEMD160_Update(ripemd160_ctx, hash256, SHA256_DIGEST_LENGTH);
  RIPEMD160_Final(hash160_tmp.uc, ripemd160_ctx);

  /* save result to global struct */
  memcpy(hash160_uncmp.uc, hash160_tmp.uc, 20);

  /* quick and dirty public key compression */
  pub_chr[0] = 0x02 | (pub_chr[64] & 0x01);

  /* compute hash160 for compressed public key */
  /* sha256(pub) */
  SHA256_Init(sha256_ctx);
  SHA256_Update(sha256_ctx, pub_chr, 33);
  SHA256_Final(hash256, sha256_ctx);
  /* ripemd160(sha256(pub)) */
  RIPEMD160_Init(ripemd160_ctx);
  RIPEMD160_Update(ripemd160_ctx, hash256, SHA256_DIGEST_LENGTH);
  RIPEMD160_Final(hash160_tmp.uc, ripemd160_ctx);

  /* save result to global struct */
  memcpy(hash160_compr.uc, hash160_tmp.uc, 20);

  return 0;
}

static int pass2hash160(unsigned char *pass, size_t pass_sz) {
  //brainflayer_init_globals();

  /* privkey = sha256(passphrase) */
  SHA256_Init(sha256_ctx);
  SHA256_Update(sha256_ctx, pass, pass_sz);
  SHA256_Final(priv256, sha256_ctx);

  return priv2hash160(priv256);
}

static int hexpass2hash160(unsigned char *hpass, size_t hpass_sz) {
  return pass2hash160(unhex(hpass, hpass_sz, unhexed, sizeof(unhexed)), hpass_sz>>1);
}

static int hexpriv2hash160(unsigned char *hpriv, size_t hpriv_sz) {
  return priv2hash160(unhex(hpriv, hpriv_sz, priv256, sizeof(priv256)));
}

static unsigned char *kdfsalt;
static size_t kdfsalt_sz;

static int warppass2hash160(unsigned char *pass, size_t pass_sz) {
  int ret;
  if ((ret = warpwallet(pass, pass_sz, kdfsalt, kdfsalt_sz, priv256)) != 0) return ret;
  pass[pass_sz] = 0;
  return priv2hash160(priv256);
}

static int bwiopass2hash160(unsigned char *pass, size_t pass_sz) {
  int ret;
  if ((ret = brainwalletio(pass, pass_sz, kdfsalt, kdfsalt_sz, priv256)) != 0) return ret;
  pass[pass_sz] = 0;
  return priv2hash160(priv256);
}

static int brainv2pass2hash160(unsigned char *pass, size_t pass_sz) {
  unsigned char hexout[33];
  int ret;
  if ((ret = brainv2(pass, pass_sz, kdfsalt, kdfsalt_sz, hexout)) != 0) return ret;
  pass[pass_sz] = 0;
  return pass2hash160(hexout, sizeof(hexout)-1);
}

static unsigned char *kdfpass;
static size_t kdfpass_sz;

static int warpsalt2hash160(unsigned char *salt, size_t salt_sz) {
  int ret;
  if ((ret = warpwallet(kdfpass, kdfpass_sz, salt, salt_sz, priv256)) != 0) return ret;
  salt[salt_sz] = 0;
  return priv2hash160(priv256);
}

static int bwiosalt2hash160(unsigned char *salt, size_t salt_sz) {
  int ret;
  if ((ret = brainwalletio(kdfpass, kdfpass_sz, salt, salt_sz, priv256)) != 0) return ret;
  salt[salt_sz] = 0;
  return priv2hash160(priv256);
}

static int brainv2salt2hash160(unsigned char *salt, size_t salt_sz) {
  unsigned char hexout[33];
  int ret;
  if ((ret = brainv2(kdfpass, kdfpass_sz, salt, salt_sz, hexout)) != 0) return ret;
  salt[salt_sz] = 0;
  return pass2hash160(hexout, sizeof(hexout)-1);
}

// function pointer
static int (*input2hash160)(unsigned char *, size_t);

inline static void fprintresult(FILE *f, hash160_t *hash,
                                unsigned char compressed,
                                unsigned char *type,
                                unsigned char *input) {
  fprintf(f, "%08x%08x%08x%08x%08x:%c:%s:%s\n",
          ntohl(hash->ul[0]),
          ntohl(hash->ul[1]),
          ntohl(hash->ul[2]),
          ntohl(hash->ul[3]),
          ntohl(hash->ul[4]),
          compressed,
          type,
          input);
}

inline static void fprintlookup(FILE *f,
                                hash160_t *hashu,
                                hash160_t *hashc,
                                unsigned char *priv,
                                unsigned char *type,
                                unsigned char *input) {
  fprintf(f, "%08x%08x%08x%08x%08x:%08x%08x%08x%08x%08x:%s:%s:%s\n",
          ntohl(hashu->ul[0]),
          ntohl(hashu->ul[1]),
          ntohl(hashu->ul[2]),
          ntohl(hashu->ul[3]),
          ntohl(hashu->ul[4]),
          ntohl(hashc->ul[0]),
          ntohl(hashc->ul[1]),
          ntohl(hashc->ul[2]),
          ntohl(hashc->ul[3]),
          ntohl(hashc->ul[4]),
          hex(priv, 32, hexed, sizeof(hexed)),
          type,
          input);
}

void usage(unsigned char *name) {
  printf("Usage: %s [OPTION]...\n\n\
 -a                          open output file in append mode\n\
 -b FILE                     check for matches against bloom filter FILE\n\
 -L                          use single line mode for table output\n\
 -i FILE                     read from FILE instead of stdin\n\
 -o FILE                     write to FILE instead of stdout\n\
 -t TYPE                     inputs are TYPE - supported types:\n\
                             str (default) - classic brainwallet passphrases\n\
                             hex  - classic brainwallets (hex encoded)\n\
                             priv - hex encoded private keys\n\
                             warp - WarpWallet (supports -s or -p)\n\
                             bwio - brainwallet.io (supports -s or -p)\n\
                             bv2  - brainv2 (supports -s or -p) VERY SLOW\n\
 -s SALT                     use SALT for salted input types (default: none)\n\
 -p PASSPHRASE               use PASSPHRASE for salted input types, inputs\n\
                             will be treated as salts\n\
 -k K                        skip the first K lines of input\n\
 -n K/N                      use only the Kth of every N input lines\n\
 -w WINDOW_SIZE              window size for ecmult table (default: 16)\n\
                             uses about 3 * 2^w KiB memory on startup, but\n\
                             only about 2^w KiB once the table is built\n\
 -m FILE                     load ecmult table from FILE\n\
                             the ecmtabgen tool can build such a table\n\
 -v                          verbose - display cracking progress\n\
 -h                          show this help\n", name);
//q, --quiet                 suppress non-error messages
  exit(1);
}

int main(int argc, char **argv) {
  FILE *ifile = stdin;
  FILE *ofile = stdout;

  int ret;

  float alpha, ilines_rate, ilines_rate_avg;
  int64_t raw_lines = -1;
  uint64_t report_mask = 0;
  uint64_t time_last, time_curr, time_delta;
  uint64_t time_start, time_elapsed;
  uint64_t ilines_last, ilines_curr, ilines_delta;
  uint64_t olines;

  int skipping = 0;

  char *line = NULL;
  size_t line_sz = 0;
  int line_read;

  int c, spok = 0, aopt = 0, vopt = 0, wopt = 16, Lopt = 0;
  int nopt_mod = 0, nopt_rem = 0;
  uint64_t kopt = 0;
  unsigned char *bopt = NULL, *iopt = NULL, *oopt = NULL;
  unsigned char *topt = NULL, *sopt = NULL, *popt = NULL;
  unsigned char *mopt = NULL;

  while ((c = getopt(argc, argv, "avb:hi:k:m:n:o:p:s:t:w:L")) != -1) {
    switch (c) {
      case 'a':
        aopt = 1; // open output file in append mode
        break;
      case 'k':
        kopt = strtoull(optarg, NULL, 10); // skip first k lines of input
        skipping = 1;
        break;
      case 'n':
        // only try the rem'th of every mod lines (one indexed)
        nopt_rem = atoi(optarg) - 1;
        optarg = strchr(optarg, '/');
        if (optarg != NULL) { nopt_mod = atoi(optarg+1); }
        skipping = 1;
        break;
      case 'w':
        if (wopt > 1) wopt = atoi(optarg);
        break;
      case 'm':
        mopt = optarg; // table file
        wopt = 1; // auto
        break;
      case 'v':
        vopt = 1; // verbose
        break;
      case 'b':
        bopt = optarg; // bloom filter file
        break;
      case 'i':
        iopt = optarg; // input file
        break;
      case 'o':
        oopt = optarg; // output file
        break;
      case 's':
        sopt = optarg; // salt
        break;
      case 'p':
        popt = optarg; // passphrase
        break;
      case 't':
        topt = optarg; // type of input
        break;
      case 'L':
        Lopt = 1; // lookup output
        break;
      case 'h':
        // show help
        usage(argv[0]);
        return 0;
      case '?':
        // show error
        return 1;
      default:
        // should never be reached...
        printf("got option '%c' (%d)\n", c, c);
        return 1;
    }
  }

  if (optind < argc) {
    if (optind == 1 && argc == 2) {
      // older versions of brainflayer had the bloom filter file as a
      // single optional argument, this keeps compatibility with that
      bopt = argv[1];
    } else {
      fprintf(stderr, "Invalid arguments:\n");
      while (optind < argc) {
        fprintf(stderr, "    '%s'\n", argv[optind++]);
      }
      exit(1);
    }
  }

  if (nopt_rem != 0 || nopt_mod != 0) {
    // note that nopt_rem has had one subtracted at option parsing
    if (nopt_rem >= nopt_mod) {
      bail(1, "Invalid '-n' argument, remainder '%d' must be <= modulus '%d'\n", nopt_rem+1, nopt_mod);
    } else if (nopt_rem < 0) {
      bail(1, "Invalid '-n' argument, remainder '%d' must be > 0\n", nopt_rem+1);
    } else if (nopt_mod < 1) {
      bail(1, "Invalid '-n' argument, modulus '%d' must be > 0\n", nopt_mod);
    }
  }

  if (wopt < 1 || wopt > 28) {
    bail(1, "Invalid window size '%d' - must be >= 1 and <= 28\n", wopt);
  } else {
    // very rough sanity check of window size
    struct sysinfo info;
    sysinfo(&info);
    uint64_t sysram = info.mem_unit * info.totalram;
    if (3584LLU*(1<<wopt) > sysram) {
      bail(1, "Not enough ram for requested window size '%d'\n", wopt);
    }
  }

  if (topt != NULL) {
    if (strcmp(topt, "str") == 0) {
      input2hash160 = &pass2hash160;
    } else if (strcmp(topt, "hex") == 0) {
      input2hash160 = &hexpass2hash160;
    } else if (strcmp(topt, "priv") == 0) {
      input2hash160 = &hexpriv2hash160;
    } else if (strcmp(topt, "warp") == 0) {
      spok = 1;
      input2hash160 = popt ? &warpsalt2hash160 : &warppass2hash160;
    } else if (strcmp(topt, "bwio") == 0) {
      spok = 1;
      input2hash160 = popt ? &bwiosalt2hash160 : &bwiopass2hash160;
    } else if (strcmp(topt, "bv2") == 0) {
      spok = 1;
      input2hash160 = popt ? &brainv2salt2hash160 : &brainv2pass2hash160;
    } else {
      bail(1, "Unknown input type '%s'.\n", topt);
    }
  } else {
    topt = "str";
    input2hash160 = &pass2hash160;
  }

  if (spok) {
    if (sopt && popt) {
      bail(1, "Cannot specify both a salt and a passphrase\n");
    }
    if (popt) {
      kdfpass = popt;
      kdfpass_sz = strlen(popt);
    } else {
      if (sopt) {
        kdfsalt = sopt;
        kdfsalt_sz = strlen(kdfsalt);
      } else {
        kdfsalt = malloc(0);
        kdfsalt_sz = 0;
      }
    }
  } else {
    if (popt) {
      bail(1, "Specifying a passphrase not supported with input type '%s'\n", topt);
    } else if (sopt) {
      bail(1, "Specifying a salt not supported with this input type '%s'\n", topt);
    }
  }

  if (bopt) {
    if (Lopt) {
      bail(1, "The '-L' option cannot be used with a bloom filter\n");
    }
    if ((ret = mmapf(&bloom_mmapf, bopt, BLOOM_SIZE, MMAPF_RNDRD)) != MMAPF_OKAY) {
      bail(1, "failed to open bloom filter '%s': %s\n", bopt, mmapf_strerror(ret));
    } else if (bloom_mmapf.mem == NULL) {
      bail(1, "got NULL pointer trying to set up bloom filter\n");
    }
    bloom = bloom_mmapf.mem;
  }

  if (iopt && (ifile = fopen(iopt, "r")) == NULL) {
    bail(1, "failed to open '%s' for reading: %s\n", iopt, strerror(errno));
  }

  if (oopt && (ofile = fopen(oopt, (aopt ? "a" : "w"))) == NULL) {
    bail(1, "failed to open '%s' for writing: %s\n", oopt, strerror(errno));
  }

  /* use line buffered output */
  setvbuf(ofile,  NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  brainflayer_init_globals();
  secp256k1_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

  if (secp256k1_ec_pubkey_precomp_table(wopt, mopt) != 0) {
    bail(1, "failed to initialize precomputed table\n");
  }

  if (vopt) {
    /* initialize timing data */
    time_start = time_last = getns();
    olines = ilines_last = ilines_curr = 0;
    ilines_rate_avg = -1;
    alpha = 0.500;
  } else {
    time_start = time_last = 0; // prevent compiler warning about uninitialized use
  }

  for (;;) {
    if ((line_read = getline(&line, &line_sz, ifile)-1) > -1) {
      if (skipping) {
        ++raw_lines;
        if (kopt && raw_lines < kopt) { continue; }
        if (nopt_mod && raw_lines % nopt_mod != nopt_rem) { continue; }
      }
      line[strlen(line)-1] = 0;
      input2hash160(line, strlen(line));
      //line[line_read] = 0;
      //input2hash160(line, line_read);
      if (bloom) {
        if (bloom_chk_hash160(bloom, hash160_uncmp.ul)) {
          if (vopt && ofile == stdout) fprintf(ofile, "\033[0K");
          fprintresult(ofile, &hash160_uncmp, 'u', topt, line);
          ++olines;
        }
        if (bloom_chk_hash160(bloom, hash160_compr.ul)) {
          if (vopt && ofile == stdout) fprintf(ofile, "\033[0K");
          fprintresult(ofile, &hash160_compr, 'c', topt, line);
          ++olines;
        }
      } else if (Lopt) {
        fprintlookup(ofile, &hash160_uncmp, &hash160_compr, priv256, topt, line);
      } else {
        fprintresult(ofile, &hash160_uncmp, 'u', topt, line);
        fprintresult(ofile, &hash160_compr, 'c', topt, line);
      }
    } else {
      if (!vopt) break;
    }

    if (vopt) {
      ++ilines_curr;
      if (line_read < 0 || (ilines_curr & report_mask) == 0) {
        time_curr = getns();
        time_delta = time_curr - time_last;
        time_elapsed = time_curr - time_start;
        time_last = time_curr;
        ilines_delta = ilines_curr - ilines_last;
        ilines_last = ilines_curr;
        ilines_rate = (ilines_delta * 1000000000.0) / (time_delta * 1.0);
        if (ilines_rate_avg < 0) {
          ilines_rate_avg = ilines_rate;
        } else {
          /* exponetial moving average */
          ilines_rate_avg = alpha * ilines_rate + (1 - alpha) * ilines_rate_avg;
        }
        /* target reporting frequency to about once every five seconds */
        if (time_delta < 2500000000) {
          report_mask = (report_mask << 1) | 1;
          ilines_rate_avg = ilines_rate; /* reset EMA */
        } else if (time_delta > 10000000000) {
          report_mask >>= 1;
          ilines_rate_avg = ilines_rate; /* reset EMA */
        }
        fprintf(stderr,
            "\033[0G\033[2K"
            " rate: %9.2f c/s"
            " found: %5zu/%-10zu"
            " elapsed: %8.3fs"
            "\033[0G",
            ilines_rate_avg,
            olines,
            ilines_curr,
            time_elapsed / 1000000000.0
        );
        fflush(stderr);
        if (line_read < 0) {
          fprintf(stderr, "\n");
          break;
        }
      }
    }
  }

  return 0;
}

/*  vim: set ts=2 sw=2 et ai si: */
