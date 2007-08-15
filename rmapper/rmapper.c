/*	$Id$	*/

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "fasta.h"
#include "rmapper.h"
#include "sw-full.h"
#include "sw-vector.h"
#include "util.h"

/* External parameters */
static u_int kmer_len		= DEF_KMER_LEN;
static u_int window_len		= DEF_WINDOW_LEN;
static u_int num_matches	= DEF_NUM_MATCHES;
static u_int taboo_len		= DEF_TABOO_LEN;
static u_int num_outputs	= DEF_NUM_OUTPUTS;

static int   match_value	= DEF_MATCH_VALUE;
static int   mismatch_value	= DEF_MISMATCH_VALUE;
static int   gap_open		= DEF_GAP_OPEN;
static int   gap_extend		= DEF_GAP_EXTEND;
static u_int sw_threshold	= DEF_SW_THRESHOLD;

/* Genomic sequence, stored in 32-bit words, first is in the LSB */
static uint32_t *genome;
static uint32_t	 genome_len;

/* Reads */
static struct read_elem  *reads;		/* list of reads */
static struct read_node **readmap;		/* kmer to read map */

static u_int	nreads;				/* total reads loaded */
static u_int	nkmers;				/* total kmers of reads loaded*/

static u_int	max_read_len;			/* longest read we've seen */

/* Flags */
static int bflag = 0;				/* print a progress bar */
static int pflag = 0;				/* pretty print results */
static int qflag = 1;				/* qsort output */

static size_t
power(size_t base, size_t exp)
{
	size_t i, ret;

	if (exp == 0)
		return (1);

	ret = base;
	for (i = 1; i < exp; i++)
		ret *= base;

	return (ret);
}

/* qsort callback - descending score, increasing index */
static int
qsort_scores(const void *a, const void *b)
{
	const struct re_score *sa, *sb;

	sa = a;
	sb = b;

	if (sa->score < sb->score)
		return (1);

	if (sa->score > sb->score)
		return (-1);

	if (sa->index > sb->index)
		return (1);

	if (sa->index < sb->index)
		return (-1);

	/* impossible */
	assert(0);

	return (0);
}

/* percolate down in our min-heap */
static void
reheap(struct re_score *scores, int node)
{
	struct re_score tmp;
	int left, right, max;

	left  = node * 2;
	right = left + 1;
	max   = node;
	
	if (left <= scores[0].score &&
	    scores[left].score < scores[node].score)
		max = left;

	if (right <= scores[0].score &&
	    scores[right].score < scores[max].score)
		max = right;

	if (max != node) {
		tmp = scores[node];
		scores[node] = scores[max];
		scores[max] = tmp;
		reheap(scores, max);
	}
}

/* update our score min-heap */
static void
save_score(struct read_elem *re, int score, int index)
{
	struct re_score *scores;

	scores = re->scores;

	if (score < scores[1].score)
		return;

	scores[1].score = score;
	scores[1].index = index;
	reheap(scores, 1);
}

static void
progress_bar(uint32_t at, uint32_t of)
{
	static int lastperc, beenhere;
	static char whirly = '\\';

	char progbuf[52];
	int perc, i, j;

	perc = (at * 100) / of;

	if (beenhere && perc - lastperc == 0)
		return;

	beenhere = 1;
	lastperc = perc;

	/* any excuse to have a whirly gig */
	switch (whirly) {
	case '|':
		whirly = '/';
		break;
	case '/':
		whirly = '-';
		break;
	case '-':
		whirly = '\\';
		break;
	case '\\':
		whirly = '|';
		break;
	}
	progbuf[25] = whirly;
		
	for (i = j = 0; i <= 100; i += 2) {
		if (j != 25) {
			if (i <= perc)
				progbuf[j++] = '=';
			else
				progbuf[j++] = ' ';
		} else {
			j++;
		}
	}
	progbuf[51] = '\0';

	fprintf(stderr, "\rProgress: [%s] %3d%%", progbuf, perc);
	fflush(stderr);
}

/* scan the genome by kmers, running S-W as needed, and updating scores */
static void
scan()
{
	struct read_node *rn;
	struct read_elem *re;
	uint32_t i, idx, kmer, kmer_mask;
	uint32_t base, skip = 0, score;
	uint32_t goff, glen;
	int prevhit;

	kmer = EXTRACT(genome, 0);
	kmer = (kmer << 2) | EXTRACT(genome, 1);
	kmer = (kmer << 2) | EXTRACT(genome, 2);

	kmer_mask = 0xffffffff >> (32 - (kmer_len * 2));

	for (i = kmer_len - 1; i < genome_len; i++) {
		if (bflag)
			progress_bar(i, genome_len);

		base = EXTRACT(genome, i);
		kmer = (kmer << 2) | base;
		kmer &= kmer_mask;

		/*
		 * If this is an invalid colour (our representation of 'N' in
		 * colour space), then don't use this kmer.
		 *
		 * XXX - currently not possible! 2 bits per colour!
		 */
		if (base == 4)
			skip = kmer_len - 1;

		if (skip > 0) {
			skip--;
			continue;
		}

		idx = i - (kmer_len - 1);

		for (rn = readmap[kmer]; rn != NULL; rn = rn->next) {
			re = rn->read;

			prevhit = re->hits[re->prev_hit];
			if ((idx - prevhit) <= taboo_len && prevhit != -1)
				continue;

			re->hits[re->next_hit] = idx;
			re->prev_hit = re->next_hit;
			re->next_hit = (re->next_hit + 1) % num_matches;

			if ((idx - re->hits[re->next_hit]) < window_len &&
			    (i - re->last_swhit_idx) >= window_len &&
			    re->hits[re->next_hit] != -1) {
				if (i < window_len)
					goff = 0;
				else
					goff = i - window_len;

				if (goff + (window_len * 2) >= genome_len)
					glen = genome_len - goff;
				else
					glen = window_len * 2;

				score = sw_vector(genome, goff, glen,
				    re->read, re->read_len);

				if (score >= sw_threshold) {
					save_score(re, score, i);
					re->last_swhit_idx = i;
					re->swhits++;
				}
			}
		}
	}

	if (bflag) {
		progress_bar(i, genome_len);
		fprintf(stderr, "\n\n");
	}
}

static void
load_genome_helper(int base, ssize_t offset, int isnewentry, char *name)
{
	static int first = 1;
	uint32_t word;
	
	/* shut up icc */
	(void)name;

	/* handle initial call to alloc resources */
	if (base == -1) {
		ssize_t alloclen;

		alloclen = (offset + 3) / 4;

		genome = malloc(alloclen);
		if (genome == NULL) {
			fprintf(stderr, "error: malloc failed: %s\n",
			    strerror(errno));
			exit(1);
		}
		memset(genome, 0, alloclen);
		return;
	}

	if (isnewentry && !first) {
		fprintf(stderr, "error: genome file consists of more than one "
		    "reftig!\n");
		exit(1);
	}

	/* XXX - we have no means of representing 'N' */

	word = genome[offset >> 4];
	word &= ~(0x3 << (2 * (offset & 15)));
	word |= (base << (2 * (offset & 15)));
	genome[offset >> 4] = word;
	genome_len++;
	first = 0;
}

static int
load_genome(const char *file)
{
	ssize_t ret;

	ret = load_fasta(file, load_genome_helper);

	if (ret != -1)
		fprintf(stderr, "Loaded %u colours of genome\n",
		    (unsigned int)ret);

	return (ret == -1);
}

static void
load_reads_helper(int base, ssize_t offset, int isnewentry, char *name)
{
	static struct read_elem *re = NULL;
	static uint32_t word, kmer, kmer_mask, cnt, skip;
	static uint32_t past_kmers[MAX_KMERS_PER_READ];
	static uint32_t npast_kmers;
	int i;

	/* shut up icc */
	(void)offset;

	/* handle initial call to alloc resources */
	if (base == -1) {
		size_t alloclen;

		alloclen = sizeof(struct read_node *) * power(4, kmer_len);
		readmap = malloc(alloclen);
		if (readmap == NULL) {
			perror("load_reads_helper: malloc failed");
			exit(1);
		}
		memset(readmap, 0, alloclen);
		return;
	}

	if (isnewentry) {
		re = malloc(sizeof(struct read_elem) +
		    (sizeof(re->hits[0]) * num_matches));
		if (re == NULL) {
			perror("load_reads_helper: malloc failed");
			exit(1);
		}

		memset(re, 0, sizeof(struct read_elem) +
                    (sizeof(re->hits[0]) * num_matches));
		re->name = strdup(name);
		memset(re->read, 0, sizeof(re->read));
		memset(re->hits, -1, sizeof(re->hits[0]) * num_matches);

		re->scores = malloc(sizeof(struct re_score) * (num_outputs +1));
		if (re->scores == NULL) {
			perror("load_reads_helper: malloc failed");
			exit(1);
		}
		re->scores[0].score = num_outputs;
		re->scores[0].index = 0;
		for (i = 1; i <= re->scores[0].score; i++)
			re->scores[i].score = 0x80000000 + i;

		word = kmer = cnt = npast_kmers = 0;
		re->next = reads;
		reads = re;
		nreads++;

		/* Throw out the first number as it depends on the marker */
		return;
	}

	/*
	 * If this is an invalid colour (our representation of 'N' in
	 * colour space), then don't use this kmer.
	 */
	if (base == 4)
		skip = kmer_len;
	
	if (re->read_len == 32) {
		fprintf(stderr, "error: read [%s] exceeds 32 characters\n",
		    re->name);
		exit(1);
	}

	kmer_mask = 0xffffffff >> (32 - (kmer_len * 2));
	kmer = (kmer << 2) & kmer_mask;
	kmer |= base;

	word = re->read[re->read_len >> 4];
	word &= ~(0x3 << (2 * (re->read_len & 15)));
	word |= (base << (2 * (re->read_len & 15)));
	re->read[re->read_len >> 4] = word;
	re->read_len++;
	max_read_len = MAX(re->read_len, max_read_len);

	if (skip == 0 && ++cnt >= kmer_len) {
		struct read_node *rn;
		int unique = 1;

		/*
		 * Ensure that this kmer is unique within the read. If it's
		 * not, we don't want to point from one kmer to the same
		 * read multiple times.
	 	 */
		assert(npast_kmers <= MAX_KMERS_PER_READ);

		for (i = 0; i < npast_kmers; i++) {
			if (past_kmers[i] == word) {
				unique = 0;
				break;
			}
		}
		
		if (unique) {
			rn = malloc(sizeof(struct read_node));
			if (rn == NULL) {
				perror("load_reads_helper: malloc failed");
				exit(1);
			}
			
			rn->read = re;
			rn->next = readmap[kmer];
			readmap[kmer] = rn;
			past_kmers[npast_kmers++] = word;
			nkmers++;
		}
	}

	if (skip > 0)
		skip--;
}

static int
load_reads(const char *file)
{
	ssize_t ret;

	ret = load_fasta(file, load_reads_helper);

	if (ret != -1)
		fprintf(stderr, "Loaded %u colours in %u reads (%u kmers)\n",
		    (unsigned int)ret, nreads, nkmers);

	return (ret == -1);
}

/* this is an unholy mess */
static void
print_pretty(struct read_elem *re)
{
	struct sw_full_results sfr;
	char *dbalign, *qralign;
	int i, j, len;
	uint32_t goff, glen, aoff;

	printf("-----------------------------------------------------------\n");
	printf("READ: [%s]\n", re->name);
	printf("-----------------------------------------------------------\n");

	for (i = 1; i <= re->scores[0].score; i++) {
		if (re->scores[i].score < 0)
			continue;

		putchar('\n');
		if (re->scores[i].index < window_len)
			goff = 0;
		else
			goff = re->scores[i].index - window_len;

		if (goff + (window_len * 2) >= genome_len)
			glen = genome_len - goff;
		else
			glen = window_len * 2;

		sw_full(genome, goff, glen, re->read, re->read_len,
		    re->scores[i].score, &dbalign, &qralign, &sfr);
		aoff = sfr.genome_start;

		len = strlen(dbalign);
		assert(len == strlen(qralign));

		printf("Score:   %d   (read start/end: %d/%d)\n",
		    re->scores[i].score, sfr.read_start,
		    sfr.read_start + sfr.mapped - 1);
		printf("Index:   %u\n", goff + aoff);
		
		printf("Reftig:  ");
		for (j = 4; j > 0; j--) {
			if (j <= goff + aoff)
				putchar('0' + EXTRACT(genome, goff + aoff - j));
		}
		printf("%s", dbalign);
		for (j = 0; j < 4; j++) {
			if ((goff + aoff + len + j) < genome_len)
				putchar('0' + EXTRACT(genome,
				    goff + aoff + len + j));
		}
		putchar('\n');

		printf("Match:   ");
		for (j = 4; j > 0; j--) {
			if (j <= goff + aoff)
				putchar(' ');
		}
		for (j = 0; j < len; j++) {
			if (dbalign[j] == qralign[j] && dbalign[j] != '-') {
				putchar('|');
			} else {
				assert(j != 0);
				putchar(' ');
			}
		}
		putchar('\n');

		printf("Read:    ");
		for (j = 4; j > 0; j--) {
			if (j <= goff + aoff)
				putchar(' ');
		}
		printf("%s\n", qralign);

	}
	putchar('\n');
	putchar('\n');

}

static void
print_normal(struct read_elem *re)
{
	struct sw_full_results sfr;
	uint32_t goff, glen;
	int i;

	for (i = 1; i <= re->scores[0].score; i++) {
		if (re->scores[i].score < 0)
			continue;

		if (re->scores[i].index < window_len)
			goff = 0;
		else
			goff = re->scores[i].index - window_len;

		if (goff + (window_len * 2) >= genome_len)
			glen = genome_len - goff;
		else
			glen = window_len * 2;

		sw_full(genome, goff, glen, re->read, re->read_len,
		    re->scores[i].score, NULL, NULL, &sfr);

		printf("[%s] %d %u %d %d %d %d %d %d\n", re->name, sfr.score,
		    goff + sfr.genome_start,sfr.read_start, sfr.mapped,
		    sfr.matches, sfr.mismatches, sfr.insertions, sfr.deletions);
	}
	putchar('\n');
}

static void
usage(char *progname)
{
	char *slash;

	slash = strrchr(progname, '/');
	if (slash != NULL)
		progname = slash + 1;

	fprintf(stderr, "usage: %s [parameters] [options] "
	    "genome_file reads_file\n", progname);

	fprintf(stderr, "parameters:\n");

	fprintf(stderr,
	    "    -k    kmer length                             (default: %d)\n",
	    DEF_KMER_LEN);

	fprintf(stderr,
	    "    -n    kmer matches per window                 (default: %d)\n",
	    DEF_NUM_MATCHES);

	fprintf(stderr,
	    "    -t    kmer taboo length                       (default: %d)\n",
	    DEF_TABOO_LEN);

	fprintf(stderr,
	    "    -w    kmer window length                      (default: %d)\n",
	    DEF_WINDOW_LEN);

	fprintf(stderr,
	    "    -o    maximum hits per read                   (default: %d)\n",
	    DEF_NUM_OUTPUTS);

	fprintf(stderr,
	    "    -m    S-W match value                         (default: %d)\n",
	    DEF_MATCH_VALUE);

	fprintf(stderr,
	    "    -i    S-W mismatch value                      (default: %d)\n",
	    DEF_MISMATCH_VALUE);

	fprintf(stderr,
	    "    -g    S-W gap open penalty                    (default: %d)\n",
	    DEF_GAP_OPEN);

	fprintf(stderr,
	    "    -e    S-W gap extend penalty                  (default: %d)\n",
	    DEF_GAP_EXTEND);

	fprintf(stderr,
	    "    -s    S-W hit threshold                       (default: %d)\n",
	    DEF_SW_THRESHOLD);

	fprintf(stderr, "\n");
	fprintf(stderr, "options:\n");

	fprintf(stderr,
	    "    -b    Print scan progress bar                 (default: "
	    "disabled)\n"); 

	fprintf(stderr,
	    "    -p    Pretty print alignments                 (default: "
	    "disabled)\n"); 

	exit(1);
}

int
main(int argc, char **argv)
{
	struct timeval tv1, tv2;
	char *genome_file, *reads_file, *progname;
	struct read_elem *re;
	double cellspersec;
	uint64_t invocs, cells;
	int ch, hits = 0;

	progname = argv[0];

	while ((ch = getopt(argc, argv, "k:n:t:w:o:m:i:g:e:s:pb")) != -1) {
		switch (ch) {
		case 'k':
			kmer_len = atoi(optarg);
			break;
		case 'n':
			num_matches = atoi(optarg);
			break;
		case 't':
			taboo_len = atoi(optarg);
			break;
		case 'w':
			window_len = atoi(optarg);
			break;
		case 'o':
			num_outputs = atoi(optarg);
			break;
		case 'm':
			match_value = atoi(optarg);
			break;
		case 'i':
			mismatch_value = atoi(optarg);
			break;
		case 'g':
			gap_open = atoi(optarg);
			break;
		case 'e':
			gap_extend = atoi(optarg);
			break;
		case 's':
			sw_threshold = atoi(optarg);
			break;
		case 'b':
			bflag = 1;
			break;
		case 'p':
			pflag = 1;
			break;
		default:
			usage(progname);
		}
	}
	argc -= optind;
	argv += optind;
	
	if (argc != 2)
		usage(progname);

	genome_file = argv[0];
	reads_file  = argv[1];

	if (kmer_len < 1 || kmer_len > 16) {
		fprintf(stderr, "error: invalid kmer length\n");
		exit(1);
	}

	if (window_len < 1 || window_len < kmer_len) {
		fprintf(stderr, "error: invalid window length\n");
		exit(1);
	}

	if (num_matches < 1) {
		fprintf(stderr, "error: invalid number of matches\n");
		exit(1);
	}

	if (taboo_len > window_len) {
		fprintf(stderr, "error: invalid taboo length\n");
		exit(1);
	}

	if (num_outputs < 1) {
		fprintf(stderr, "error: invalid maximum hits per read\n");
		exit(1);
	}

	if (gap_open > 0) {
		fprintf(stderr, "error: invalid gap open penalty\n");
		exit(1);
	}

	if (gap_extend > 0) {
		fprintf(stderr, "error: invalid gap extend penalty\n");
		exit(1);
	}

	if (load_genome(genome_file)) {
		exit(1);
	}

	if (load_reads(reads_file)) {
		exit(1);
	}

	if (sw_vector_setup(window_len * 2, max_read_len,
	    gap_open, gap_extend, match_value, mismatch_value)) {
		fprintf(stderr, "failed to initialise vector "
		    "Smith-Waterman (%s)\n", strerror(errno));
		exit(1);
	}

	if (sw_full_setup(window_len * 2, max_read_len,
	    gap_open, gap_extend, match_value, mismatch_value)) {
		fprintf(stderr, "failed to initialise scalar "
		    "Smith-Waterman (%s)\n", strerror(errno));
		exit(1);
	}

	fprintf(stderr, "Settings:\n");
	fprintf(stderr, "    kmer length:                %u\n", kmer_len);
	fprintf(stderr, "    kmer matches per window:    %u\n", num_matches);
	fprintf(stderr, "    kmer taboo length:          %u\n", taboo_len);
	fprintf(stderr, "    kmer window length:         %u\n", window_len);
	fprintf(stderr, "    maximum hits per read:      %u\n", num_outputs);
	fprintf(stderr, "    S-W match value:            %d\n", match_value);
	fprintf(stderr, "    S-W mismatch value:         %d\n", mismatch_value);
	fprintf(stderr, "    S-W gap open penalty:       %d\n", gap_open);
	fprintf(stderr, "    S-W gap extend penalty:     %d\n", gap_extend);
	fprintf(stderr, "    S-W hit threshold:          %u\n", sw_threshold);
	fprintf(stderr, "\n");

	gettimeofday(&tv1, NULL);
	scan();
	gettimeofday(&tv2, NULL);

	if (!pflag) {
		printf("# [READ_NAME] score index read_start "
		    "read_mapped_length matches mismatches "
		    "insertions deletions\n");
	}

	for (re = reads; re != NULL; re = re->next) {
		if (re->swhits == 0)
			continue;

		if (qflag) {
			qsort(&re->scores[1], re->scores[0].score,
			    sizeof(re->scores[0]), qsort_scores);
		}

		if (pflag)
			print_pretty(re);
		else
			print_normal(re);

		hits++;
	}

	sw_vector_stats(&invocs, &cells, NULL, &cellspersec);
	
	fprintf(stderr, "Statistics:\n");
	fprintf(stderr, "    Kmer Scan:\n");
	fprintf(stderr, "        Run-time:               %.2f seconds\n",
	    ((double)tv2.tv_sec + (double)tv2.tv_usec / 1.0e6) -
	    ((double)tv1.tv_sec + (double)tv1.tv_usec / 1.0e6) -
	    (double)cells / cellspersec);
	fprintf(stderr, "        Total Kmers:            %u\n", nkmers);
	fprintf(stderr, "\n");

	fprintf(stderr, "    Vector Smith-Waterman:\n");
	fprintf(stderr, "        Run-time:               %.2f seconds\n",
	    cells / cellspersec);
	fprintf(stderr, "        Invocations:            %" PRIu64 "\n",invocs);
	fprintf(stderr, "        Cells Computed:         %.2f million\n",
	    (double)cells / 1.0e6);
	fprintf(stderr, "        Cells per Second:       %.2f million\n",
	    cellspersec / 1.0e6);
	fprintf(stderr, "\n");

	sw_full_stats(&invocs, &cells, NULL, &cellspersec);

	fprintf(stderr, "    Scalar Smith-Waterman:\n");
	fprintf(stderr, "        Run-time:               %.2f seconds\n",
	    cells / cellspersec);
	fprintf(stderr, "        Invocations:            %" PRIu64 "\n",invocs);
	fprintf(stderr, "        Cells Computed:         %.2f million\n",
	    (double)cells / 1.0e6);
	fprintf(stderr, "        Cells per Second:       %.2f million\n",
	    cellspersec / 1.0e6);
	fprintf(stderr, "\n");

	fprintf(stderr, "    General:\n");
	fprintf(stderr, "        Reads matched:          %d\n", hits);
	
	return (0);
}
