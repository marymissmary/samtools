/*  bam_plcmd.c -- mpileup subcommand.

    Copyright (C) 2008-2014 Genome Research Ltd.
    Portions copyright (C) 2009-2012 Broad Institute.

    Author: Heng Li <lh3@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

// marohrdanz (mar4) 8 December 2014
// reconfiguring the -l option so that it uses Chris's performance improvements.
// Basically making the -l option into the --MULTIPILEUP option

// marohrdanz 3 nov 2014
// recreating Chris's modifications to include BAM cacheing
// (from version 0.1.19) into this version 1.0

#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <getopt.h>
#include <htslib/sam.h>
#include <htslib/faidx.h>
#include <htslib/kstring.h>
#include <htslib/khash_str2int.h>
#include "sam_header.h"
#include "samtools.h"
#include <htslib/bgzf.h>   // mar4: <-- for declaration of bgzf_set_cache_size

static inline int printw(int c, FILE *fp)
{
    char buf[16];
    int l, x;
    if (c == 0) return fputc('0', fp);
    for (l = 0, x = c < 0? -c : c; x > 0; x /= 10) buf[l++] = x%10 + '0';
    if (c < 0) buf[l++] = '-';
    buf[l] = 0;
    for (x = 0; x < l/2; ++x) {
        int y = buf[x]; buf[x] = buf[l-1-x]; buf[l-1-x] = y;
    }
    fputs(buf, fp);
    return 0;
}

static inline void pileup_seq(FILE *fp, const bam_pileup1_t *p, int pos, int ref_len, const char *ref)
{
    int j;
    if (p->is_head) {
        putc('^', fp);
        putc(p->b->core.qual > 93? 126 : p->b->core.qual + 33, fp);
    }
    if (!p->is_del) {
        int c = p->qpos < p->b->core.l_qseq
            ? seq_nt16_str[bam_seqi(bam_get_seq(p->b), p->qpos)]
            : 'N';
        if (ref) {
            int rb = pos < ref_len? ref[pos] : 'N';
            if (c == '=' || seq_nt16_table[c] == seq_nt16_table[rb]) c = bam_is_rev(p->b)? ',' : '.';
            else c = bam_is_rev(p->b)? tolower(c) : toupper(c);
        } else {
            if (c == '=') c = bam_is_rev(p->b)? ',' : '.';
            else c = bam_is_rev(p->b)? tolower(c) : toupper(c);
        }
        putc(c, fp);
    } else putc(p->is_refskip? (bam_is_rev(p->b)? '<' : '>') : '*', fp);
    if (p->indel > 0) {
        putc('+', fp); printw(p->indel, fp);
        for (j = 1; j <= p->indel; ++j) {
            int c = seq_nt16_str[bam_seqi(bam_get_seq(p->b), p->qpos + j)];
            putc(bam_is_rev(p->b)? tolower(c) : toupper(c), fp);
        }
    } else if (p->indel < 0) {
        printw(p->indel, fp);
        for (j = 1; j <= -p->indel; ++j) {
            int c = (ref && (int)pos+j < ref_len)? ref[pos+j] : 'N';
            putc(bam_is_rev(p->b)? tolower(c) : toupper(c), fp);
        }
    }
    if (p->is_tail) putc('$', fp);
}

#include <assert.h>
#include "bam2bcf.h"
#include "sample.h"

#define MPLP_BCF        1
#define MPLP_VCF        (1<<1)
#define MPLP_NO_COMP    (1<<2)
#define MPLP_NO_ORPHAN  (1<<3)
#define MPLP_REALN      (1<<4)
#define MPLP_NO_INDEL   (1<<5)
#define MPLP_REDO_BAQ   (1<<6)
#define MPLP_ILLUMINA13 (1<<7)
#define MPLP_IGNORE_RG  (1<<8)
#define MPLP_PRINT_POS  (1<<9)
#define MPLP_PRINT_MAPQ (1<<10)
#define MPLP_PER_SAMPLE (1<<11)
#define MPLP_SMART_OVERLAPS (1<<12)

void *bed_read(const char *fn);
int bed_read_as_array(const char *bedfilename, int *pnlines, char ***region_lines);
void bed_destroy(void *_h);
int bed_overlap(const void *_h, const char *chr, int beg, int end);


// mar4: here is where we would define Chris's new structs/typedef:
// mar4: mplp_filecache_t

typedef struct {
  char *fname;
  samFile *fp;      // mar4: this was bamFile in 0.1.19
  bam_hdr_t *h;    // mar4: this was bam_header_t in 0.1.19
  hts_idx_t *idx;  // mar4: this was bam_index_t in 0.1.19
} mplp_filecache_t;


typedef struct {
    int min_mq, flag, min_baseQ, capQ_thres, max_depth, max_indel_depth, fmt_flag;
    int rflag_require, rflag_filter;
    int openQ, extQ, tandemQ, min_support; // for indels
    int bamcachesizemb;   // mar4: 
    int regbegin, regend; // mar4: beginning and end of region
    double min_frac; // for indels
    char *reg, *pl_list, *fai_fname, *output_fname;
    faidx_t *fai;
    void *bed, *rghash;
    mplp_filecache_t *filecache;  // mar4: add file cache to this structure
    int argc;
    char **argv;
} mplp_conf_t;

typedef struct {
    samFile *fp;
    hts_itr_t *iter;
    bam_hdr_t *h;
    int ref_id;
    char *ref;
    const mplp_conf_t *conf;
} mplp_aux_t;

typedef struct {
    int n;
    int *n_plp, *m_plp;
    bam_pileup1_t **plp;
} mplp_pileup_t;

// mar4: add in Chris's profiling variables:
static int cw_count1 = 0;
static int cw_bam_iter_read_counter = 0;
static int cw_out_of_region_count = 0;


static int mplp_func(void *data, bam1_t *b)
{
    cw_count1 += 1;
    extern int bam_realn(bam1_t *b, const char *ref);
    extern int bam_prob_realn_core(bam1_t *b, const char *ref, int);
    extern int bam_cap_mapQ(bam1_t *b, char *ref, int thres);
    mplp_aux_t *ma = (mplp_aux_t*)data;
    int ret, skip = 0;
    do {
        int has_ref;
        // mar4: commenting the following line out, and putting in some of Chris's profiling,
        // mar4: but making changes: bam_iter_read -> sam_iter_next, and 
        // mar4: bam_read1 -> sam_read1
        //ret = ma->iter? sam_itr_next(ma->fp, ma->iter, b) : sam_read1(ma->fp, ma->h, b);
        if (ma->iter) {
          ret = sam_itr_next(ma->fp,ma->iter,b);
          cw_bam_iter_read_counter += 1;
        } else {
          //ret = bam_read1(ma->fp,b);
          ret = sam_read1(ma->fp,ma->h,b);
        }
        // the following lines are back to non-Chris-mod code:

        if (ret < 0) break;
        // The 'B' cigar operation is not part of the specification, considering as obsolete.
        //  bam_remove_B(b);
        if (b->core.tid < 0 || (b->core.flag&BAM_FUNMAP)) { // exclude unmapped reads
            skip = 1;
            continue;
        }
        if (ma->conf->rflag_require && !(ma->conf->rflag_require&b->core.flag)) { skip = 1; continue; }
        if (ma->conf->rflag_filter && ma->conf->rflag_filter&b->core.flag) { skip = 1; continue; }
        if (ma->conf->bed) { // test overlap
            skip = !bed_overlap(ma->conf->bed, ma->h->target_name[b->core.tid], b->core.pos, bam_endpos(b));
            if (skip) continue;
        }
        if (ma->conf->rghash) { // exclude read groups
            uint8_t *rg = bam_aux_get(b, "RG");
            skip = (rg && khash_str2int_get(ma->conf->rghash, (const char*)(rg+1), NULL)==0);
            if (skip) continue;
        }
        if (ma->conf->flag & MPLP_ILLUMINA13) {
            int i;
            uint8_t *qual = bam_get_qual(b);
            for (i = 0; i < b->core.l_qseq; ++i)
                qual[i] = qual[i] > 31? qual[i] - 31 : 0;
        }
        has_ref = (ma->ref && ma->ref_id == b->core.tid)? 1 : 0;
        skip = 0;
        if (has_ref && (ma->conf->flag&MPLP_REALN)) bam_prob_realn_core(b, ma->ref, (ma->conf->flag & MPLP_REDO_BAQ)? 7 : 3);
        if (has_ref && ma->conf->capQ_thres > 10) {
            int q = bam_cap_mapQ(b, ma->ref, ma->conf->capQ_thres);
            if (q < 0) skip = 1;
            else if (b->core.qual > q) b->core.qual = q;
        }
        if (b->core.qual < ma->conf->min_mq) skip = 1;
        else if ((ma->conf->flag&MPLP_NO_ORPHAN) && (b->core.flag&BAM_FPAIRED) && !(b->core.flag&BAM_FPROPER_PAIR)) skip = 1;
    } while (skip);
    return ret;
}

static void group_smpl(mplp_pileup_t *m, bam_sample_t *sm, kstring_t *buf,
                       int n, char *const*fn, int *n_plp, const bam_pileup1_t **plp, int ignore_rg)
{
    int i, j;
    memset(m->n_plp, 0, m->n * sizeof(int));
    for (i = 0; i < n; ++i) {
        for (j = 0; j < n_plp[i]; ++j) {
            const bam_pileup1_t *p = plp[i] + j;
            uint8_t *q;
            int id = -1;
            q = ignore_rg? 0 : bam_aux_get(p->b, "RG");
            if (q) id = bam_smpl_rg2smid(sm, fn[i], (char*)q+1, buf);
            if (id < 0) id = bam_smpl_rg2smid(sm, fn[i], 0, buf);
            if (id < 0 || id >= m->n) {
                assert(q); // otherwise a bug
                fprintf(stderr, "[%s] Read group %s used in file %s but absent from the header or an alignment missing read group.\n", __func__, (char*)q+1, fn[i]);
                exit(1);
            }
            if (m->n_plp[id] == m->m_plp[id]) {
                m->m_plp[id] = m->m_plp[id]? m->m_plp[id]<<1 : 8;
                m->plp[id] = realloc(m->plp[id], sizeof(bam_pileup1_t) * m->m_plp[id]);
            }
            m->plp[id][m->n_plp[id]++] = *p;
        }
    }
}

/*
 * Performs pileup
 * @param conf configuration for this pileup
 * @param n number of files specified in fn
 * @param fn filenames
 */
static int mpileup(mplp_conf_t *conf, int n, char **fn)
{
    extern void *bcf_call_add_rg(void *rghash, const char *hdtext, const char *list);
    extern void bcf_call_del_rghash(void *rghash);
    mplp_aux_t **data;
    int i, tid, pos, *n_plp, tid0 = -1, beg0 = 0, end0 = 1u<<29, ref_len, ref_tid = -1, max_depth, max_indel_depth;
    const bam_pileup1_t **plp;
    bam_mplp_t iter;
    bam_hdr_t *h = NULL; /* header of first file in input list */
    char *ref;
    void *rghash = NULL;
    FILE *pileup_fp = NULL;

    bcf_callaux_t *bca = NULL;
    bcf_callret1_t *bcr = NULL;
    bcf_call_t bc;
    htsFile *bcf_fp = NULL;
    bcf_hdr_t *bcf_hdr = NULL;

    bam_sample_t *sm = NULL;
    kstring_t buf;
    mplp_pileup_t gplp;

    memset(&gplp, 0, sizeof(mplp_pileup_t));
    memset(&buf, 0, sizeof(kstring_t));
    memset(&bc, 0, sizeof(bcf_call_t));
    data = calloc(n, sizeof(mplp_aux_t*));
    plp = calloc(n, sizeof(bam_pileup1_t*));
    n_plp = calloc(n, sizeof(int));
    sm = bam_smpl_init();

    // mar4: some more of Chris's profiling variables:
    cw_count1 = 0;
    // cw_bam_read1_count = 0;
    cw_bam_iter_read_counter = 0;

    if (n == 0) {
        fprintf(stderr,"[%s] no input file/data given\n", __func__);
        exit(1);
    }

    // read the header of each file in the list and initialize data
    for (i = 0; i < n; ++i) {
        bam_hdr_t *h_tmp;
        data[i] = calloc(1, sizeof(mplp_aux_t));
        // mar4: here is the place to put some of Chris's mods, the ones that start
        // mar4: on line 270 in /workspace/cwakefield/ken/mod-samtools/bam_plcmd.c
        //
        // mar4: here is the 1.0 version:
        //   data[i]->fp = sam_open(fn[i], "rb");  // mar4: <-- different in 1.0 vs 0.1.19
        // mar4: here is the 0.1.19 version:
        //    data[i]->fp = strcmp(fn[i], "-") == 0? bam_dopen(fileno(stdin), "r") : bam_open(fn[i], "r");
        // mar4: adding in the file cachine lines:
        if (conf->filecache && conf->filecache[i].fp != 0) {
          data[i]->fp = conf->filecache[i].fp;
        } else { // mar4: do the original thing
          // mar4: this is the original line, as it would have been in 0.1.19....but things have
          // mar4: changed a bit...so I think the correct line here is the second one down
          // data[i]->fp = strcmp(fn[i],"-") == 0? bam_dopen(fileno(stdin),"r") : bam_open(fn[i], "r");        
          // mar4: for some reason, compiler doesn't like: data[i]->fp = sam_open(fn[i],"rb"); because of 
          // mar4: samtools_sam_open definition in bam.h...so changing to:
           data[i]->fp = sam_open(fn[i],"rb"); // mar4: <-- back to original, w/o #incluce "bam.h"
          if ( !data[i]->fp )
          {
             fprintf(stderr, "[%s] failed to open %s: %s\n", __func__, fn[i], strerror(errno));
             exit(1);
          }
          if (conf->filecache && conf->bamcachesizemb)
          {
            conf->filecache[i].fp = data[i]->fp;
            // mar4: cw comment: Set a BGZF cache
            //fprintf(stderr, "Setting BGZF cache size to %d M\n", conf->bamcachesizemb);
            //bgzf_set_cache_size(data[i]->fp, conf->bamcachesizemb * 1024 * 1024);
            bgzf_set_cache_size(data[i]->fp->fp.bgzf, conf->bamcachesizemb * 1024 * 1024);
          } else if (n == 1) { // mar4: cw comment: this doesn't seem to help when doing mpileup w/o bed regions
            //fprintf(stderr, "Setting BGZF cache size to %d M\n", conf->bamcachesizemb);
            //bgzf_set_cache_size(data[i]->fp, conf->bamcachesizemb * 1024 * 1024);
            bgzf_set_cache_size(data[i]->fp->fp.bgzf, conf->bamcachesizemb * 1024 * 1024);
          }
        }
    
        // mar4: back to original code
        hts_set_fai_filename(data[i]->fp, conf->fai_fname); // mar4: <-- new in 1.0
        data[i]->conf = conf;
        //h_tmp = sam_hdr_read(data[i]->fp);
        //if ( !h_tmp ) {
        //    fprintf(stderr,"[%s] fail to read the header of %s\n", __func__, fn[i]);
        //    exit(1);
       // }
        // mar4: Chris's filecaching:
        if (conf->filecache && conf->filecache[i].h != 0) {
          h_tmp = conf->filecache[i].h;
        } else {
          //h_tmp = bam_header_read(data[i]->fp); // mar4: <-- for 0.1.19
          h_tmp = sam_hdr_read(data[i]->fp);   // mar4: new 1.0 version
          if ( !h_tmp) {
            fprintf(stderr,"[%s] fail to read header of %s\n",__func__,fn[i]);
            exit(1);
          }
          if (conf->filecache) conf->filecache[i].h = h_tmp;
        }
        // mar4: back to normal code
        data[i]->h = i? h : h_tmp; // for i==0, "h" has not been set yet
        bam_smpl_add(sm, fn[i], (conf->flag&MPLP_IGNORE_RG)? 0 : h_tmp->text);
        // Collect read group IDs with PL (platform) listed in pl_list (note: fragile, strstr search)
        rghash = bcf_call_add_rg(rghash, h_tmp->text, conf->pl_list);
        if (conf->reg) {
            // mar4: hts_idx_t in 1.0 was bam_index_t in 0.1.19
            //hts_idx_t *idx = sam_index_load(data[i]->fp, fn[i]);
            hts_idx_t *idx;
            if (conf->filecache && conf->filecache[i].idx != 0) {
              idx = conf->filecache[i].idx;
            } else {
              idx = bam_index_load(fn[i]);
              if (conf->filecache) conf->filecache[i].idx = idx;
            }
        

            if (idx == 0) {
                fprintf(stderr, "[%s] fail to load index for %s\n", __func__, fn[i]);
                exit(1);
            }
            // mar4: this check existed in 0.1.19, but not in 1.0.
            // mar4: I'm going to put it in for now, and see what happens:
            //if (bam_parse_region(h_tmp, conf->reg, &tid, &conf->regbegin, &conf->regend) < 0) {
            //  fprintf(stderr,"[%s] malformatted region or wrong sequence for %s\n",__func__,fn[i]);
            //  exit(1);
            //}
            // mar4: this check didn't exist in 0.1.19....
            if ( (data[i]->iter=sam_itr_querys(idx, data[i]->h, conf->reg)) == 0) {
                fprintf(stderr,"mar4: conf->reg: %s\n",conf->reg);
                fprintf(stderr, "[E::%s] fail to parse region '%s'\n", __func__, conf->reg);
                exit(1);
            }
            // mar4: there is a different line in Chris's mods:
            //  if (i == 0) tid0 = tid, beg0 = conf->regbegin, end0 = conf->regend; 
            // mar4: I'm going to leave what was originally in 1.0 for now:
            if (i == 0) tid0 = data[i]->iter->tid, beg0 = data[i]->iter->beg, end0 = data[i]->iter->end;
            if (!conf->filecache) hts_idx_destroy(idx); // mar4: Chris added conditional...original code was just to destroy
        }
        if (i == 0) {
           h = h_tmp; /* save the header of first file in list */
        } else {
            // FIXME: to check consistency
            // mar4: again, adding the filecache conditional here:
            if (!conf->filecache) bam_hdr_destroy(h_tmp);
        }
    }
    // allocate data storage proportionate to number of samples being studied sm->n
    gplp.n = sm->n;
    gplp.n_plp = calloc(sm->n, sizeof(int));
    gplp.m_plp = calloc(sm->n, sizeof(int));
    gplp.plp = calloc(sm->n, sizeof(bam_pileup1_t*));

    fprintf(stderr, "[%s] %d samples in %d input files\n", __func__, sm->n, n);
    // write the VCF header
    if (conf->flag & MPLP_BCF)
    {
        const char *mode;
        if ( conf->flag & MPLP_VCF )
            mode = (conf->flag&MPLP_NO_COMP)? "wu" : "wz";   // uncompressed VCF or compressed VCF
        else
            mode = (conf->flag&MPLP_NO_COMP)? "wub" : "wb";  // uncompressed BCF or compressed BCF

        bcf_fp = bcf_open(conf->output_fname? conf->output_fname : "-", mode);
        if (bcf_fp == NULL) {
            fprintf(stderr, "[%s] failed to write to %s: %s\n", __func__, conf->output_fname? conf->output_fname : "standard output", strerror(errno));
            exit(1);
        }

        bcf_hdr = bcf_hdr_init("w");
        kstring_t str = {0,0,0};

        ksprintf(&str, "##samtoolsVersion=%s+htslib-%s\n",samtools_version(),hts_version());
        bcf_hdr_append(bcf_hdr, str.s);

        str.l = 0;
        ksprintf(&str, "##samtoolsCommand=samtools mpileup");
        for (i=1; i<conf->argc; i++) ksprintf(&str, " %s", conf->argv[i]);
        kputc('\n', &str);
        bcf_hdr_append(bcf_hdr, str.s);

        if (conf->fai_fname)
        {
            str.l = 0;
            ksprintf(&str, "##reference=file://%s\n", conf->fai_fname);
            bcf_hdr_append(bcf_hdr, str.s);
        }

        // todo: use/write new BAM header manipulation routines, fill also UR, M5
        for (i=0; i<h->n_targets; i++)
        {
            str.l = 0;
            ksprintf(&str, "##contig=<ID=%s,length=%d>", h->target_name[i], h->target_len[i]);
            bcf_hdr_append(bcf_hdr, str.s);
        }
        free(str.s);
        bcf_hdr_append(bcf_hdr,"##ALT=<ID=X,Description=\"Represents allele(s) other than observed.\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=INDEL,Number=0,Type=Flag,Description=\"Indicates that the variant is an INDEL.\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=IDV,Number=1,Type=Integer,Description=\"Maximum number of reads supporting an indel\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=IMF,Number=1,Type=Float,Description=\"Maximum fraction of reads supporting an indel\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=DP,Number=1,Type=Integer,Description=\"Raw read depth\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=VDB,Number=1,Type=Float,Description=\"Variant Distance Bias for filtering splice-site artefacts in RNA-seq data (bigger is better)\",Version=\"3\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=RPB,Number=1,Type=Float,Description=\"Mann-Whitney U test of Read Position Bias (bigger is better)\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=MQB,Number=1,Type=Float,Description=\"Mann-Whitney U test of Mapping Quality Bias (bigger is better)\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=BQB,Number=1,Type=Float,Description=\"Mann-Whitney U test of Base Quality Bias (bigger is better)\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=MQSB,Number=1,Type=Float,Description=\"Mann-Whitney U test of Mapping Quality vs Strand Bias (bigger is better)\">");
#if CDF_MWU_TESTS
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=RPB2,Number=1,Type=Float,Description=\"Mann-Whitney U test of Read Position Bias [CDF] (bigger is better)\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=MQB2,Number=1,Type=Float,Description=\"Mann-Whitney U test of Mapping Quality Bias [CDF] (bigger is better)\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=BQB2,Number=1,Type=Float,Description=\"Mann-Whitney U test of Base Quality Bias [CDF] (bigger is better)\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=MQSB2,Number=1,Type=Float,Description=\"Mann-Whitney U test of Mapping Quality vs Strand Bias [CDF] (bigger is better)\">");
#endif
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=SGB,Number=1,Type=Float,Description=\"Segregation based metric.\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=MQ0F,Number=1,Type=Float,Description=\"Fraction of MQ0 reads (smaller is better)\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=I16,Number=16,Type=Float,Description=\"Auxiliary tag used for calling, see description of bcf_callret1_t in bam2bcf.h\">");
        bcf_hdr_append(bcf_hdr,"##INFO=<ID=QS,Number=R,Type=Float,Description=\"Auxiliary tag used for calling\">");
        bcf_hdr_append(bcf_hdr,"##FORMAT=<ID=PL,Number=G,Type=Integer,Description=\"List of Phred-scaled genotype likelihoods\">");
        if ( conf->fmt_flag&B2B_FMT_DP )
            bcf_hdr_append(bcf_hdr,"##FORMAT=<ID=DP,Number=1,Type=Integer,Description=\"Number of high-quality bases\">");
        if ( conf->fmt_flag&B2B_FMT_DV )
            bcf_hdr_append(bcf_hdr,"##FORMAT=<ID=DV,Number=1,Type=Integer,Description=\"Number of high-quality non-reference bases\">");
        if ( conf->fmt_flag&B2B_FMT_DPR )
            bcf_hdr_append(bcf_hdr,"##FORMAT=<ID=DPR,Number=R,Type=Integer,Description=\"Number of high-quality bases observed for each allele\">");
        if ( conf->fmt_flag&B2B_INFO_DPR )
            bcf_hdr_append(bcf_hdr,"##INFO=<ID=DPR,Number=R,Type=Integer,Description=\"Number of high-quality bases observed for each allele\">");
        if ( conf->fmt_flag&B2B_FMT_DP4 )
            bcf_hdr_append(bcf_hdr,"##FORMAT=<ID=DP4,Number=4,Type=Integer,Description=\"Number of high-quality ref-fwd, ref-reverse, alt-fwd and alt-reverse bases\">");
        if ( conf->fmt_flag&B2B_FMT_SP )
            bcf_hdr_append(bcf_hdr,"##FORMAT=<ID=SP,Number=1,Type=Integer,Description=\"Phred-scaled strand bias P-value\">");

        for (i=0; i<sm->n; i++)
            bcf_hdr_add_sample(bcf_hdr, sm->smpl[i]);
        bcf_hdr_add_sample(bcf_hdr, NULL);
        bcf_hdr_write(bcf_fp, bcf_hdr);

        bca = bcf_call_init(-1., conf->min_baseQ);
        bcr = calloc(sm->n, sizeof(bcf_callret1_t));
        bca->rghash = rghash;
        bca->openQ = conf->openQ, bca->extQ = conf->extQ, bca->tandemQ = conf->tandemQ;
        bca->min_frac = conf->min_frac;
        bca->min_support = conf->min_support;
        bca->per_sample_flt = conf->flag & MPLP_PER_SAMPLE;

        bc.bcf_hdr = bcf_hdr;
        bc.n = sm->n;
        bc.PL = malloc(15 * sm->n * sizeof(*bc.PL));
        if (conf->fmt_flag)
        {
            assert( sizeof(float)==sizeof(int32_t) );
            bc.DP4 = malloc(sm->n * sizeof(int32_t) * 4);
            bc.fmt_arr = malloc(sm->n * sizeof(float)); // all fmt_flag fields
            if ( conf->fmt_flag&(B2B_INFO_DPR|B2B_FMT_DPR) )
            {
                // first B2B_MAX_ALLELES fields for total numbers, the rest per-sample
                bc.DPR = malloc((sm->n+1)*B2B_MAX_ALLELES*sizeof(int32_t));
                for (i=0; i<sm->n; i++)
                    bcr[i].DPR = bc.DPR + (i+1)*B2B_MAX_ALLELES;
            }
        }
    }
    else {
        pileup_fp = conf->output_fname? fopen(conf->output_fname, "w") : stdout;

        if (pileup_fp == NULL) {
            fprintf(stderr, "[%s] failed to write to %s: %s\n", __func__, conf->output_fname, strerror(errno));
            exit(1);
        }
    }

    if (tid0 >= 0 && conf->fai) { // region is set mar4: cw comment: ('-f' option)
        ref = faidx_fetch_seq(conf->fai, h->target_name[tid0], 0, 0x7fffffff, &ref_len);
        ref_tid = tid0;
        for (i = 0; i < n; ++i) data[i]->ref = ref, data[i]->ref_id = tid0;
    } else ref_tid = -1, ref = 0;

    // begin pileup
    iter = bam_mplp_init(n, mplp_func, (void**)data);
    if ( conf->flag & MPLP_SMART_OVERLAPS ) bam_mplp_init_overlaps(iter);
    max_depth = conf->max_depth;
    if (max_depth * sm->n > 1<<20)
        fprintf(stderr, "(%s) Max depth is above 1M. Potential memory hog!\n", __func__);
    if (max_depth * sm->n < 8000) {
        max_depth = 8000 / sm->n;
        fprintf(stderr, "<%s> Set max per-file depth to %d\n", __func__, max_depth);
    }
    // mar4: some more of Chris's profiling and comments:
    // cw: report read counts prior to this point --> all zeros
    int cw_pos_lt_beg0 = 0;
    int cw_pos_ge_end0 = 0;
    int cw_bam_mplp_auto_count = 0;


    max_indel_depth = conf->max_indel_depth * sm->n;
    bam_mplp_set_maxcnt(iter, max_depth);
    bcf1_t *bcf_rec = bcf_init1();  // mar4: <-- new to 1.0
    int ret;                        // mar4: <-- new to 1.0
    while ( (ret=bam_mplp_auto(iter, &tid, &pos, n_plp, plp)) > 0) {
        //fprintf(stderr,"mar4: pos: %i\n",pos);
        //fprintf(stderr,"mar4: pos+1: %i\n",pos+1);
        cw_bam_mplp_auto_count += 1;
        if (conf->reg && (pos < beg0 || pos >= end0)) {
          cw_out_of_region_count += 1;
          // mar4: cw comment: count loops pos < beg0 verses >= end0
          if (pos < beg0)  cw_pos_lt_beg0 += 1;
          if (pos >= end0) cw_pos_ge_end0 += 1;
          continue; // out of the region requested mar4: <-- original command
        }
        if (conf->bed && tid >= 0 && !bed_overlap(conf->bed, h->target_name[tid], pos, pos+1)) continue;
        if (tid != ref_tid) {
            //fprintf(stderr,"cw print: In tid != ref_tid. tid = %d ; ref_tid = %d\n",tid,ref_tid);
            free(ref); ref = 0;
            if (conf->fai) ref = faidx_fetch_seq(conf->fai, h->target_name[tid], 0, 0x7fffffff, &ref_len);
            for (i = 0; i < n; ++i) data[i]->ref = ref, data[i]->ref_id = tid;
            ref_tid = tid;
        }
        if (conf->flag & MPLP_BCF) { // if generating BCF output (genotype likelihoods)
            int total_depth, _ref0, ref16;
            for (i = total_depth = 0; i < n; ++i) total_depth += n_plp[i];
            group_smpl(&gplp, sm, &buf, n, fn, n_plp, plp, conf->flag & MPLP_IGNORE_RG);
            _ref0 = (ref && pos < ref_len)? ref[pos] : 'N';
            ref16 = seq_nt16_table[_ref0];
            bcf_callaux_clean(bca, &bc);
            for (i = 0; i < gplp.n; ++i)
                bcf_call_glfgen(gplp.n_plp[i], gplp.plp[i], ref16, bca, bcr + i);
            bc.tid = tid; bc.pos = pos;
            bcf_call_combine(gplp.n, bcr, bca, ref16, &bc);
            bcf_clear1(bcf_rec);
            bcf_call2bcf(&bc, bcf_rec, bcr, conf->fmt_flag, 0, 0);
            bcf_write1(bcf_fp, bcf_hdr, bcf_rec);
            // call indels; todo: subsampling with total_depth>max_indel_depth instead of ignoring?
            if (!(conf->flag&MPLP_NO_INDEL) && total_depth < max_indel_depth && bcf_call_gap_prep(gplp.n, gplp.n_plp, gplp.plp, pos, bca, ref, rghash) >= 0)
            {
                bcf_callaux_clean(bca, &bc);
                for (i = 0; i < gplp.n; ++i)
                    bcf_call_glfgen(gplp.n_plp[i], gplp.plp[i], -1, bca, bcr + i);
                if (bcf_call_combine(gplp.n, bcr, bca, -1, &bc) >= 0) {
                    bcf_clear1(bcf_rec);
                    bcf_call2bcf(&bc, bcf_rec, bcr, conf->fmt_flag, bca, ref);
                    bcf_write1(bcf_fp, bcf_hdr, bcf_rec);
                }
            }
        } else {
            fprintf(pileup_fp, "%s\t%d\t%c", h->target_name[tid], pos + 1, (ref && pos < ref_len)? ref[pos] : 'N');
            int qualc[20000]; // mar4: <-- cw addition
            for (i = 0; i < n; ++i) {
                if (n_plp[i] > 20000) {
                  fprintf(stderr,"cw print: PROBLEM ??? n_plp[%d] = %d\n",i,n_plp[i]);
                }
                int j, cnt;
                for (j = cnt = 0; j < n_plp[i]; ++j) {
                    const bam_pileup1_t *p = plp[i] + j;
                    // slight restructuring in 1.0:
                    //int c = p->qpos < p->b->core.l_qseq  // mar4: slight resturcure difference in 1.0
                    //         ? bam_get_qual(p->b)[p->qpos]
                    //         : 0;
                    //if (c >= conf->min_baseQ) ++cnt;
                    // mar4: some lines from chris.... in 0.1.19...
                    //qualc[j] = bam1_qual(p->b)[p->qpos];
                    // if (qualc[j] >= conf->min_baseQ) ++cnt;
                    // mar4: making Chris's 0.1.19 mods in 1.0 format:
                    qualc[j] = p->qpos < p->b->core.l_qseq ? bam_get_qual(p->b)[p->qpos] : 0;
                    //fprintf(stderr,"mar4: 1.0: qualc[j] = %i[%i]\n",qualc[j],j);
                    if (qualc[j] >= conf->min_baseQ) ++cnt;
                    
                } 
                fprintf(pileup_fp, "\t%d\t", cnt);
                if (n_plp[i] == 0) {
                    fputs("*\t*", pileup_fp);
                    if (conf->flag & MPLP_PRINT_MAPQ) fputs("\t*", pileup_fp);
                    if (conf->flag & MPLP_PRINT_POS) fputs("\t*", pileup_fp);
                } else {
                    for (j = 0; j < n_plp[i]; ++j) {
                        // mar4: structure slightly different from 0.1.19 also:
                        //const bam_pileup1_t *p = plp[i] + j;
                        //int c = p->qpos < p->b->core.l_qseq
                        //    ? bam_get_qual(p->b)[p->qpos]
                        //    : 0;
                        //if (c >= conf->min_baseQ)
                        // mar4: here is what was originally in 0.1.19
                        //const bam_pileup1_t *p = pop[i] + j;
                        //if (bam1_qual(p->b)[p->qpos] >= conf->min_baseQ)
                        // mar4: here is Chris's mod in 0.1.19
                        if (qualc[j] >= conf->min_baseQ)  // mar4: just using this for now
                            pileup_seq(pileup_fp, plp[i] + j, pos, ref_len, ref);
                    }
                    putc('\t', pileup_fp);
                    for (j = 0; j < n_plp[i]; ++j) {
                        // mar4: original 1.0:
                        //const bam_pileup1_t *p = plp[i] + j;
                        //int c = p->qpos < p->b->core.l_qseq
                        //    ? bam_get_qual(p->b)[p->qpos]
                        //    : 0;
                        // mar4: original 0.1.19
                        //const bam_pileup1_t *p = plp[i] + j;
                        //int c = bam1_qual(p->b)[p->qpos];
                        // mar4: Chris's mod in 0.1.19:
                        const bam_pileup1_t *p = plp[i] + j;
                        int c = p->qpos < p->b->core.l_qseq
                            ? bam_get_qual(p->b)[p->qpos]
                            : 0;
                        if (c >= conf->min_baseQ) {
                            c = c + 33 < 126? c + 33 : 126;
                            putc(c, pileup_fp);
                        }
                    }
                    if (conf->flag & MPLP_PRINT_MAPQ) {
                        putc('\t', pileup_fp);
                        for (j = 0; j < n_plp[i]; ++j) {
                            const bam_pileup1_t *p = plp[i] + j;
                            int c = bam_get_qual(p->b)[p->qpos];
                            if ( c < conf->min_baseQ ) continue;
                            c = plp[i][j].b->core.qual + 33;
                            if (c > 126) c = 126;
                            putc(c, pileup_fp);
                        }
                    }
                    if (conf->flag & MPLP_PRINT_POS) {
                        putc('\t', pileup_fp);
                        for (j = 0; j < n_plp[i]; ++j) {
                            if (j > 0) putc(',', pileup_fp);
                            fprintf(pileup_fp, "%d", plp[i][j].qpos + 1); // FIXME: printf() is very slow...
                        }
                    }
                }
            }
            putc('\n', pileup_fp);
        }
    }

    // clean up
    // mar4: not sure how much of this cleanup should be within a 
    // mar4: if (!conf->filecace) conditional....
    free(bc.tmp.s);
    bcf_destroy1(bcf_rec);
    if (bcf_fp)
    {
        hts_close(bcf_fp);
        bcf_hdr_destroy(bcf_hdr);
        bcf_call_destroy(bca);
        free(bc.PL);
        free(bc.DP4);
        free(bc.DPR);
        free(bc.fmt_arr);
        free(bcr);
    }
    if (pileup_fp && conf->output_fname) fclose(pileup_fp);
    bam_smpl_destroy(sm); free(buf.s);
    for (i = 0; i < gplp.n; ++i) free(gplp.plp[i]);
    free(gplp.plp); free(gplp.n_plp); free(gplp.m_plp);
    bcf_call_del_rghash(rghash);
    bam_mplp_destroy(iter);
    // bam_hdr_destroy(h); // mar4: <-- original
    if (!conf->filecache) bam_hdr_destroy(h);
    for (i = 0; i < n; ++i) {
        // sam_close(data[i]->fp); // mar4: <-- original
        if (!conf->filecache) sam_close(data[i]->fp);
        if (data[i]->iter) hts_itr_destroy(data[i]->iter);
        free(data[i]);
    }
    free(data); free(plp); free(ref); free(n_plp);
    // mar4: some Chris prints:
    //fprintf(stderr,"Called mplp_func %d times \n",cw_count1);
    //fprintf(stderr,"    cw_bam_iter_read_counter   = %d\n",cw_bam_iter_read_counter);
    //fprintf(stderr,"    cw_bam_mplp_auto_count     = %d\n",cw_bam_mplp_auto_count);
    //fprintf(stderr,"    cw_out_of_region_count     = %d\n",cw_out_of_region_count);
    //fprintf(stderr,"    cw_pos_lt_beg0             = %d\n",cw_pos_lt_beg0);
    //fprintf(stderr,"    cw_pos_ge_end0             = %d\n",cw_pos_ge_end0);
    return ret;
}

#define MAX_PATH_LEN 1024
int read_file_list(const char *file_list,int *n,char **argv[])
{
    char buf[MAX_PATH_LEN];
    int len, nfiles = 0;
    char **files = NULL;
    struct stat sb;

    *n = 0;
    *argv = NULL;

    FILE *fh = fopen(file_list,"r");
    if ( !fh )
    {
        fprintf(stderr,"%s: %s\n", file_list,strerror(errno));
        return 1;
    }

    files = calloc(nfiles,sizeof(char*));
    nfiles = 0;
    while ( fgets(buf,MAX_PATH_LEN,fh) )
    {
        // allow empty lines and trailing spaces
        len = strlen(buf);
        while ( len>0 && isspace(buf[len-1]) ) len--;
        if ( !len ) continue;

        // check sanity of the file list
        buf[len] = 0;
        if (stat(buf, &sb) != 0)
        {
            // no such file, check if it is safe to print its name
            int i, safe_to_print = 1;
            for (i=0; i<len; i++)
                if (!isprint(buf[i])) { safe_to_print = 0; break; }
            if ( safe_to_print )
                fprintf(stderr,"The file list \"%s\" appears broken, could not locate: %s\n", file_list,buf);
            else
                fprintf(stderr,"Does the file \"%s\" really contain a list of files and do all exist?\n", file_list);
            return 1;
        }

        nfiles++;
        files = realloc(files,nfiles*sizeof(char*));
        files[nfiles-1] = strdup(buf);
    }
    fclose(fh);
    if ( !nfiles )
    {
        fprintf(stderr,"No files read from %s\n", file_list);
        return 1;
    }
    *argv = files;
    *n    = nfiles;
    return 0;
}
#undef MAX_PATH_LEN

// mar4: new chunck of code from Chris for reading bed lines...
//
#define MAX_BED_LEN 160
int read_bed_lines (const char *bed_file_name, int *n, char **argv[])
{
  char buf[MAX_BED_LEN];
  int len, nlines = 0;
  char **lines = NULL;

  *n = 0;
  *argv = NULL;

  FILE *fh = fopen(bed_file_name,"r");
  if ( !fh) 
  {
    fprintf(stderr,"%s: %s\n", bed_file_name, strerror(errno));
    return 1;
  }

  lines = calloc(nlines,sizeof(char*));
  nlines = 0;
  while ( fgets(buf,MAX_BED_LEN,fh) )
  {
    // allow empty lines and trailing spaces
    len = strlen(buf);
    while ( len>0 && isspace(buf[len-1]) ) len--;
    if (!len) continue;
    buf[len] = 0;
    nlines++;
    lines = realloc(lines,nlines*sizeof(char*));
    lines[nlines-1] = strdup(buf);
  }
  fclose(fh);
  if (!nlines)
  {
    fprintf(stderr,"No regions read from %s\n",bed_file_name);
    return 1;
  }
  *argv = lines;
  *n    = nlines;
  return 0;
}
#undef MAX_BED_LEN
  
// mar4: another function from Chris:
void fixBedLine (char *line) 
{
  // first need to add 1 to the very first entry
  char *array[3];
  int i=0;
  array[i] = strtok(line,"\t");
  while ( array[i] != NULL )
  {
    array[++i] = strtok(NULL,"\t");
  }
  sprintf(array[1],"%d",atoi(array[1])+1);
  sprintf(line,"%s\t%s\t%s",array[0],array[1],array[2]);
  line = index(line,'\t');
  if (line) {
    *line = ':';
    line = index(line,'\t');
    if (line) *line = '-';
  }
  return;
}
// mar4: back to normal 1.0 code...

// mar4: this function not in 0.1.19:
int parse_format_flag(const char *str)
{
    int i, flag = 0, n_tags;
    char **tags = hts_readlist(str, 0, &n_tags);
    for(i=0; i<n_tags; i++)
    {
        if ( !strcasecmp(tags[i],"DP") ) flag |= B2B_FMT_DP;
        else if ( !strcasecmp(tags[i],"DV") ) flag |= B2B_FMT_DV;
        else if ( !strcasecmp(tags[i],"SP") ) flag |= B2B_FMT_SP;
        else if ( !strcasecmp(tags[i],"DP4") ) flag |= B2B_FMT_DP4;
        else if ( !strcasecmp(tags[i],"DPR") ) flag |= B2B_FMT_DPR;
        else if ( !strcasecmp(tags[i],"INFO/DPR") ) flag |= B2B_INFO_DPR;
        else
        {
            fprintf(stderr,"Could not parse tag \"%s\" in \"%s\"\n", tags[i], str);
            exit(1);
        }
        free(tags[i]);
    }
    if (n_tags) free(tags);
    return flag;
}

// mar4: this function not in 0.1.19...but its functionality is in 
// mar4: bam_mpileup.  (it's much cleaner as a separate function :).
static void print_usage(FILE *fp, const mplp_conf_t *mplp)
{
    char *tmp_require = bam_flag2str(mplp->rflag_require);
    char *tmp_filter  = bam_flag2str(mplp->rflag_filter);

    // Display usage information, formatted for the standard 80 columns.
    // (The unusual string formatting here aids the readability of this
    // source code in 80 columns, to the extent that's possible.)

    fprintf(fp,
"\n"
"Usage: samtools mpileup [options] in1.bam [in2.bam [...]]\n"
"\n"
"Input options:\n"
"  -6, --illumina1.3+      quality is in the Illumina-1.3+ encoding\n"
"  -A, --count-orphans     do not discard anomalous read pairs\n"
"  -b, --bam-list FILE     list of input BAM filenames, one per line\n"
"  -B, --no-BAQ            disable BAQ (per-Base Alignment Quality)\n"
"  -C, --adjust-MQ INT     adjust mapping quality; recommended:50, disable:0 [0]\n"
"  -d, --max-depth INT     max per-BAM depth; avoids excessive memory usage [%d]\n", mplp->max_depth);
    fprintf(fp,
"  -E, --redo-BAQ          recalculate BAQ on the fly, ignore existing BQs\n"
"  -f, --fasta-ref FILE    faidx indexed reference sequence file\n"
"  -G, --exclude-RG FILE   exclude read groups listed in FILE\n"
"  -l, --positions FILE    skip unlisted positions (chr pos) or regions (BED)\n"
"  -q, --min-MQ INT        skip alignments with mapQ smaller than INT [%d]\n", mplp->min_mq);
    fprintf(fp,
"  -Q, --min-BQ INT        skip bases with baseQ/BAQ smaller than INT [%d]\n", mplp->min_baseQ);
    fprintf(fp,
"  -r, --region REG        region in which pileup is generated\n"
"  -R, --ignore-RG         ignore RG tags (one BAM = one sample)\n"
"  --rf, --incl-flags STR|INT  required flags: skip reads with mask bits unset [%s]\n", tmp_require);
    fprintf(fp,
"  --ff, --excl-flags STR|INT  filter flags: skip reads with mask bits set\n"
"                                            [%s]\n", tmp_filter);
    fprintf(fp,
"  -x, --ignore-overlaps   disable read-pair overlap detection\n"
"\n"
"Output options:\n"
"  -o, --output FILE       write output to FILE [standard output]\n"
"  -g, --BCF               generate genotype likelihoods in BCF format\n"
"  -v, --VCF               generate genotype likelihoods in VCF format\n"
"\n"
"Output options for mpileup format (without -g/-v):\n"
"  -O, --output-BP         output base positions on reads\n"
"  -s, --output-MQ         output mapping quality\n"
"\n"
"Output options for genotype likelihoods (when -g/-v is used):\n"
"  -t, --output-tags LIST  optional tags to output: DP,DPR,DV,DP4,INFO/DPR,SP []\n"
"  -u, --uncompressed      generate uncompressed VCF/BCF output\n"
"\n"
"SNP/INDEL genotype likelihoods options (effective with -g/-v):\n"
"  -e, --ext-prob INT      Phred-scaled gap extension seq error probability [%d]\n", mplp->extQ);
    fprintf(fp,
"  -F, --gap-frac FLOAT    minimum fraction of gapped reads [%g]\n", mplp->min_frac);
    fprintf(fp,
"  -h, --tandem-qual INT   coefficient for homopolymer errors [%d]\n", mplp->tandemQ);
    fprintf(fp,
"  -I, --skip-indels       do not perform indel calling\n"
"  -L, --max-idepth INT    maximum per-sample depth for INDEL calling [%d]\n", mplp->max_indel_depth);
    fprintf(fp,
"  -m, --min-ireads INT    minimum number gapped reads for indel candidates [%d]\n", mplp->min_support);
    fprintf(fp,
"  -o, --open-prob INT     Phred-scaled gap open seq error probability [%d]\n", mplp->openQ);
    fprintf(fp,
"  -p, --per-sample-mF     apply -m and -F per-sample for increased sensitivity\n"
"  -P, --platforms STR     comma separated list of platforms for indels [all]\n"
"\n"
"Notes: Assuming diploid individuals.\n");

    free(tmp_require);
    free(tmp_filter);
}

int bam_mpileup(int argc, char *argv[])
{
    int c;
    const char *file_list = NULL;
    char **fn = NULL;
    int nfiles = 0, use_orphan = 0;
    int multipileup = 0;  // mar4: cw mod
    int few_bed_regions = 0;  // mar4: few bed regions for -l option
    mplp_conf_t mplp;
    memset(&mplp, 0, sizeof(mplp_conf_t));
    mplp.min_baseQ = 13;
    mplp.capQ_thres = 0;
    mplp.max_depth = 250; mplp.max_indel_depth = 250;
    mplp.openQ = 40; mplp.extQ = 20; mplp.tandemQ = 100;
    mplp.min_frac = 0.002; mplp.min_support = 1;
    mplp.flag = MPLP_NO_ORPHAN | MPLP_REALN | MPLP_SMART_OVERLAPS;
    mplp.argc = argc; mplp.argv = argv;
    mplp.rflag_filter = BAM_FUNMAP | BAM_FSECONDARY | BAM_FQCFAIL | BAM_FDUP;
    mplp.output_fname = NULL;
    static const struct option lopts[] =
    {
        {"rf", required_argument, NULL, 1},   // require flag
        {"ff", required_argument, NULL, 2},   // filter flag
        {"incl-flags", required_argument, NULL, 1},
        {"excl-flags", required_argument, NULL, 2},
        {"output", required_argument, NULL, 3},
        {"open-prob", required_argument, NULL, 4},
        {"illumina1.3+", no_argument, NULL, '6'},
        {"MULTIPILEUP",  required_argument, NULL, 7}, // mar4: cw multipileup mode, arg is file
        {"bamcachesize", required_argument, NULL, 8}, // mar4: cw set cachesize, arg is cachesize in MB
        {"count-orphans", no_argument, NULL, 'A'},
        {"bam-list", required_argument, NULL, 'b'},
        {"no-BAQ", no_argument, NULL, 'B'},
        {"no-baq", no_argument, NULL, 'B'},
        {"adjust-MQ", required_argument, NULL, 'C'},
        {"adjust-mq", required_argument, NULL, 'C'},
        {"max-depth", required_argument, NULL, 'd'},
        {"redo-BAQ", no_argument, NULL, 'E'},
        {"redo-baq", no_argument, NULL, 'E'},
        {"fasta-ref", required_argument, NULL, 'f'},
        {"exclude-RG", required_argument, NULL, 'G'},
        {"exclude-rg", required_argument, NULL, 'G'},
        {"positions", required_argument, NULL, 'l'},
        {"region", required_argument, NULL, 'r'},
        {"ignore-RG", no_argument, NULL, 'R'},
        {"ignore-rg", no_argument, NULL, 'R'},
        {"min-MQ", required_argument, NULL, 'q'},
        {"min-mq", required_argument, NULL, 'q'},
        {"min-BQ", required_argument, NULL, 'Q'},
        {"min-bq", required_argument, NULL, 'Q'},
        {"ignore-overlaps", no_argument, NULL, 'x'},
        {"BCF", no_argument, NULL, 'g'},
        {"bcf", no_argument, NULL, 'g'},
        {"VCF", no_argument, NULL, 'v'},
        {"vcf", no_argument, NULL, 'v'},
        {"output-BP", no_argument, NULL, 'O'},
        {"output-bp", no_argument, NULL, 'O'},
        {"output-MQ", no_argument, NULL, 's'},
        {"output-mq", no_argument, NULL, 's'},
        {"output-tags", required_argument, NULL, 't'},
        {"uncompressed", no_argument, NULL, 'u'},
        {"ext-prob", required_argument, NULL, 'e'},
        {"gap-frac", required_argument, NULL, 'F'},
        {"tandem-qual", required_argument, NULL, 'h'},
        {"skip-indels", no_argument, NULL, 'I'},
        {"max-idepth", required_argument, NULL, 'L'},
        {"min-ireads ", required_argument, NULL, 'm'},
        {"per-sample-mF", no_argument, NULL, 'p'},
        {"per-sample-mf", no_argument, NULL, 'p'},
        {"platforms", required_argument, NULL, 'P'},
        {NULL, 0, NULL, 0}
    };
    while ((c = getopt_long(argc, argv, "Agf:r:l:q:Q:uRC:BDSd:L:b:P:po:e:h:Im:F:EG:6OsVvxt:",lopts,NULL)) >= 0) {
        switch (c) {
        case 'x': mplp.flag &= ~MPLP_SMART_OVERLAPS; break;
        case  1 :
            mplp.rflag_require = bam_str2flag(optarg);
            if ( mplp.rflag_require<0 ) { fprintf(stderr,"Could not parse --rf %s\n", optarg); return 1; }
            break;
        case  2 :
            mplp.rflag_filter = bam_str2flag(optarg);
            if ( mplp.rflag_filter<0 ) { fprintf(stderr,"Could not parse --ff %s\n", optarg); return 1; }
            break;
        case  3 : mplp.output_fname = optarg; break;
        case  4 : mplp.openQ = atoi(optarg); break;
        case  7 :     // mar4: turn on Chris's mods
          mplp.bed = strdup(optarg);
          multipileup = 1;
          break;
        case  8 :    // mar4: set cache size
          mplp.bamcachesizemb = atoi(optarg);
          break;
        case 'f':
            mplp.fai = fai_load(optarg);
            if (mplp.fai == 0) return 1;
            mplp.fai_fname = optarg;
            break;
        case 'd': mplp.max_depth = atoi(optarg); break;
        case 'r': mplp.reg = strdup(optarg); break;
        case 'l':
                  // In the original version the whole BAM was streamed which is inefficient
                  //  with few BED intervals and big BAMs. Todo: devise a heuristic to determine
                  //  best strategy, that is streaming or jumping.
                  few_bed_regions = 1;  // mar4: set up to always use my new code
                  if (few_bed_regions) 
                  {
                    mplp.bed = strdup(optarg); // mar4: new code I'm adding to use cw's performance improvements
                    mplp.bamcachesizemb = 50;  // mar4: hard-code for now 
                  } 
                  else 
                  {
                    mplp.bed = bed_read(optarg);  // mar4: the original functionality
                  }
                  if (!mplp.bed) { print_error_errno("Could not read file \"%s\"", optarg); return 1; }
                  break;
        case 'P': mplp.pl_list = strdup(optarg); break;
        case 'p': mplp.flag |= MPLP_PER_SAMPLE; break;
        case 'g': mplp.flag |= MPLP_BCF; break;
        case 'v': mplp.flag |= MPLP_BCF | MPLP_VCF; break;
        case 'u': mplp.flag |= MPLP_NO_COMP | MPLP_BCF; break;
        case 'B': mplp.flag &= ~MPLP_REALN; break;
        case 'D': mplp.fmt_flag |= B2B_FMT_DP; fprintf(stderr, "[warning] samtools mpileup option `-D` is functional, but deprecated. Please switch to `-t DP` in future.\n"); break;
        case 'S': mplp.fmt_flag |= B2B_FMT_SP; fprintf(stderr, "[warning] samtools mpileup option `-S` is functional, but deprecated. Please switch to `-t SP` in future.\n"); break;
        case 'V': mplp.fmt_flag |= B2B_FMT_DV; fprintf(stderr, "[warning] samtools mpileup option `-V` is functional, but deprecated. Please switch to `-t DV` in future.\n"); break;
        case 'I': mplp.flag |= MPLP_NO_INDEL; break;
        case 'E': mplp.flag |= MPLP_REDO_BAQ; break;
        case '6': mplp.flag |= MPLP_ILLUMINA13; break;
        case 'R': mplp.flag |= MPLP_IGNORE_RG; break;
        case 's': mplp.flag |= MPLP_PRINT_MAPQ; break;
        case 'O': mplp.flag |= MPLP_PRINT_POS; break;
        case 'C': mplp.capQ_thres = atoi(optarg); break;
        case 'q': mplp.min_mq = atoi(optarg); break;
        case 'Q': mplp.min_baseQ = atoi(optarg); break;
        case 'b': file_list = optarg; break;
        case 'o': {
                char *end;
                long value = strtol(optarg, &end, 10);
                // Distinguish between -o INT and -o FILE (a bit of a hack!)
                if (*end == '\0') mplp.openQ = value;
                else mplp.output_fname = optarg;
            }
            break;
        case 'e': mplp.extQ = atoi(optarg); break;
        case 'h': mplp.tandemQ = atoi(optarg); break;
        case 'A': use_orphan = 1; break;
        case 'F': mplp.min_frac = atof(optarg); break;
        case 'm': mplp.min_support = atoi(optarg); break;
        case 'L': mplp.max_indel_depth = atoi(optarg); break;
        case 'G': {
                FILE *fp_rg;
                char buf[1024];
                mplp.rghash = khash_str2int_init();
                if ((fp_rg = fopen(optarg, "r")) == 0)
                    fprintf(stderr, "(%s) Fail to open file %s. Continue anyway.\n", __func__, optarg);
                while (!feof(fp_rg) && fscanf(fp_rg, "%s", buf) > 0) // this is not a good style, but forgive me...
                    khash_str2int_inc(mplp.rghash, strdup(buf));
                fclose(fp_rg);
            }
            break;
        case 't': mplp.fmt_flag |= parse_format_flag(optarg); break;
        default:
            fprintf(stderr,"Invalid option: '%c'\n", c);
            return 1;
        }
    }
    if ( !(mplp.flag&MPLP_REALN) && mplp.flag&MPLP_REDO_BAQ )
    {
        fprintf(stderr,"Error: The -B option cannot be combined with -E\n");
        return 1;
    }
    if (use_orphan) mplp.flag &= ~MPLP_NO_ORPHAN;
    if (argc == 1)
    {
        print_usage(stderr, &mplp);
        return 1;
    }
    int ret;
    if (file_list) {
        if ( read_file_list(file_list,&nfiles,&fn) ) return 1;
        ret = mpileup(&mplp,nfiles,fn);
        for (c=0; c<nfiles; c++) free(fn[c]);
        free(fn);
    } else if (few_bed_regions || multipileup)  { // mar4: use filecaching
      // mar4: cw comment: Loop over BED file, inserting each line into mplp.reg before mpileup
      fprintf(stderr,"mar4: few_bed_regions\n");
      const char* bedfilename = (const char*) mplp.bed;
      int nbedlines;
      char **region_lines = NULL;
      int mary = bed_read_as_array(bedfilename,&nbedlines,&region_lines);
      //fprintf(stderr,"mar4: region_lines[%i] = %s\n",ix,region_lines[ix]);
      fprintf(stderr,"mar4: back in %s, nbedlines = %i\n",__func__,nbedlines);
      //fprintf(stderr,"mar4: mplp.bed:    %s\n",mplp.bed);
      int ix;
      int numbamfiles = argc - optind;
      
      mplp.bed = (void*) 0;
      fprintf(stderr,"mar4: after mplp.bed assignment.\n");
      //if (read_bed_lines(bedfilename,&nbedlines,&fn) ) return 1;
      mplp.filecache = calloc(numbamfiles, sizeof(mplp_filecache_t));
      // mar4: cw comment: read each line in BED file, inserting into mplp.reg before mpileup
      fprintf(stderr,"mar4: before loop.\n");
      for (ix = 0; ix < nbedlines ; ix++) {
        //fixBedLine (fn[ix]);
        fprintf(stderr,"mar4: start loop...idx = %i\n",ix);
        fprintf(stderr,"mar4: region_lines[%i] = %s\n",ix,region_lines[ix]);
        mplp.reg = region_lines[ix];
        fprintf(stderr,"mar4: mplp.reg: %s\n",mplp.reg);
        fprintf(stderr,"mar4: getting ready for mpileup.\n");
        // mar4: trying ret = here... but we return something different in 1.0 vs 0.1.19
        ret = mpileup(&mplp,argc-optind,argv+optind);
      }
      mplp.reg = 0;
    } else { 
        ret = mpileup(&mplp, argc - optind, argv + optind);
    }
    if (mplp.rghash) khash_str2int_destroy_free(mplp.rghash);
    free(mplp.reg); free(mplp.pl_list);
    if (mplp.fai) fai_destroy(mplp.fai);
    if (mplp.bed) bed_destroy(mplp.bed);
    return ret;
}
