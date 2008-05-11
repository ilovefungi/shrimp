/*	$Id$	*/

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
 
#include <sys/types.h>
#include <sys/stat.h>

#include "../common/fasta.h"
#include "../common/dynhash.h"
#include "../common/sw-full-common.h"
#include "../common/sw-full-cs.h"
#include "../common/sw-full-ls.h"
#include "../common/input.h"
#include "../common/output.h"
#include "../common/util.h"
#include "../common/version.h"

#include "../rmapper/rmapper.h"		/* for External parameters below */

/* External parameters */
static int match_value    = DEF_MATCH_VALUE;
static int mismatch_value = DEF_MISMATCH_VALUE;
static int gap_open	  = DEF_GAP_OPEN;
static int gap_extend     = DEF_GAP_EXTEND;
static int xover_penalty  = DEF_XOVER_PENALTY;

static dynhash_t read_list;
static dynhash_t contig_list;

static uint64_t nread_bases;
static uint64_t ncontig_bases;
static uint32_t longest_read_len;	/* longest read we have */
static uint32_t longest_genome_len;	/* longest genome seq matched against */

/* probcalc/rmapper output cache */
struct fpo {
	struct input input;		/* input fields */
	struct fpo  *next;		/* next alignment to do */
};

/* for read and contig lists */
struct sequence {
	char	 *name;
	uint32_t *sequence;
	uint32_t  sequence_len;
	int	  initbp;		/* initial base pair (colourspace) */
	bool	  revcmpl;
};

static struct fpo *alignments;		/* forward alignments */
static uint64_t    nalignments;

static struct fpo *alignments_revcmpl;	/* reverse complement alignments */
static uint64_t    nalignments_revcmpl;

#ifdef USE_COLOURS
const bool use_colours = true;
#else
const bool use_colours = false;
#endif

static bool Rflag = false;		/* don't output read sequence */

#define MAX_READ_LEN	5000		/* ridiculously high upper bound */

static void
load_output_file(char *file)
{
	struct input inp;
	FILE *fp;
	struct fpo *fpo, *lastfpo = NULL, *lastfpo_revcmpl = NULL;

	fp = fopen(file, "r");
	if (fp == NULL) {
		fprintf(stderr, "error: failed to open probcalc/rmapper output "
		    "file [%s]: %s\n", file, strerror(errno));
		exit(1);
	}

	while (input_parseline(fp, &inp)) {
		fpo = xmalloc(sizeof(*fpo));
		memcpy(&fpo->input, &inp, sizeof(fpo->input));
		fpo->next = NULL;

		if (INPUT_IS_REVCMPL(&inp)) {
			if (lastfpo_revcmpl == NULL)
				alignments_revcmpl = fpo;
			else
				lastfpo_revcmpl->next = fpo;
			lastfpo_revcmpl = fpo;
			nalignments_revcmpl++;
		} else {
			if (lastfpo == NULL)
				alignments = fpo;
			else
				lastfpo->next = fpo;
			lastfpo = fpo;
			nalignments++;
		}

		longest_genome_len = MAX(longest_genome_len,
		   fpo->input.genome_end - fpo->input.genome_start + 1);
	}
}

static void
load_genome_file_helper(int base, ssize_t offset, int isnewentry, char *name,
    int initbp)
{
	static struct sequence *seq;
	static ssize_t maxlen;
	static uint32_t allocated;

	/* handle initial call to alloc resources */
	if (base == FASTA_ALLOC) {
		assert(seq == NULL);
		maxlen = offset;
		return;
	} else if (base == FASTA_DEALLOC) {
		seq = NULL;
		return;
	}

	if (isnewentry) {
		/* start with a small allocation: 1 word's worth */
		allocated = 8;

		seq = xmalloc(sizeof(*seq));
		seq->sequence = xmalloc(sizeof(seq->sequence[0]) *
		    BPTO32BW(allocated));
		memset(seq->sequence, 0, sizeof(seq->sequence[0]) *
		    BPTO32BW(allocated));
		seq->sequence_len = 0;
		seq->name = NULL;
		seq->revcmpl = false;
		seq->name = xstrdup(name);
		seq->initbp = initbp;

		/* add to our dynhash */
		if (dynhash_find(contig_list, seq->name, NULL, NULL)) {
			fprintf(stderr, "error: contig [%s] occurs multiple "
			    "times in the read input files\n", seq->name);
			exit(1);
		}

		if (dynhash_add(contig_list, seq->name, seq) == false) {
			fprintf(stderr, "error: failed to add contig to list - "
			    "probably out of memory\n");
			exit(1);
		}
	}

	/*
	 * Make more room, if necessary. Grow quickly at first, then slow down
	 * so as not to overshoot too much.
	 */
	if (seq->sequence_len == allocated) {
		if (BPTO32BW(allocated) * sizeof(seq->sequence[0]) >
		    64 * 1024 * 1024)
			allocated = (allocated * 4) / 3;	
		else
			allocated *= 2;

		seq->sequence = xrealloc(seq->sequence,
		    sizeof(seq->sequence[0]) * BPTO32BW(allocated));
	}

	/* shut up, icc */
	(void)maxlen;

	assert(seq->sequence_len < maxlen);
	assert(base >= 0 && base <= 15);

	bitfield_append(seq->sequence, seq->sequence_len++, base);
	ncontig_bases++;
}

static void
load_genome_file(char *fpath, struct stat *sb, void *arg)
{
	ssize_t ret;

	/* shut up, icc */
	(void)sb;
	(void)arg;

	ret = load_fasta(fpath, load_genome_file_helper, LETTER_SPACE);

	if (ret == -1) {
		fprintf(stderr, "error: failed to parse contig fasta file "
		    "[%s]\n", fpath);
		exit(1);
	}
}

static void
load_reads_file_helper(int base, ssize_t offset, int isnewentry, char *name,
    int initbp)
{
	static struct sequence *seq;
	static uint32_t *read;
	static uint32_t  read_len;

	/* shut up, icc */
	(void)offset;

	/* handle initial call to alloc resources */
	if (base == FASTA_ALLOC) {
		assert(seq == NULL);

		if (read == NULL) {
			read = xmalloc(sizeof(read[0]) *
			    BPTO32BW(MAX_READ_LEN));
			memset(read, 0, sizeof(read[0]) *
                            BPTO32BW(MAX_READ_LEN));
		}
		read_len = 0;

		return;
	} else if (base == FASTA_DEALLOC) {
		assert(read != NULL);
		if (seq != NULL) {
			seq->sequence = xmalloc(sizeof(seq->sequence[0]) *
			    BPTO32BW(read_len));
			memcpy(seq->sequence, read,
			    sizeof(seq->sequence[0]) * BPTO32BW(read_len));
			seq->sequence_len = read_len;
		}
		free(read);
		read = NULL;
		read_len = 0;
		seq = NULL;
		return;
	}

	if (isnewentry) {
		if (seq != NULL) {
			seq->sequence = xmalloc(sizeof(seq->sequence[0]) *
			    BPTO32BW(read_len));
			memcpy(seq->sequence, read,
			    sizeof(seq->sequence[0]) * BPTO32BW(read_len));
			seq->sequence_len = read_len;
		}
		seq = xmalloc(sizeof(*seq));
		seq->name = xstrdup(name);
		seq->sequence = NULL;
		seq->sequence_len = 0;
		seq->initbp = initbp;
		seq->revcmpl = false;
		memset(read, 0, sizeof(read[0]) * BPTO32BW(MAX_READ_LEN));
		read_len = 0;

		/* add to our dynhash */
		if (dynhash_find(read_list, seq->name, NULL, NULL)) {
			fprintf(stderr, "error: read [%s] occurs multiple "
			    "times in the read input files\n", seq->name);
			exit(1);
		}

		if (dynhash_add(read_list, seq->name, seq) == false) {
			fprintf(stderr, "error: failed to add read to list - "
			    "probably out of memory\n");
			exit(1);
		}
	}

	assert(seq != NULL);
	assert(read_len < MAX_READ_LEN);
	assert(base >= 0 && base <= 15);

	bitfield_append(read, read_len++, base);
	longest_read_len = MAX(longest_read_len, read_len);
	nread_bases++;
}

static void
load_reads_file(char *fpath, struct stat *sb, void *arg)
{
	ssize_t ret;

	/* shut up, icc */
	(void)sb;
	(void)arg;

	if (use_colours)
		ret = load_fasta(fpath, load_reads_file_helper, COLOUR_SPACE);
	else
		ret = load_fasta(fpath, load_reads_file_helper, LETTER_SPACE);

	if (ret == -1) {
		fprintf(stderr, "error: failed to reads parse fasta file "
		    "[%s]\n", fpath);
		exit(1);
	}
}

static uint32_t
keyhasher(void *k)
{

	return (hash_string((char *)k));
}

static int
keycomparer(void *a, void *b)
{

	return (strcmp((char *)a, (char *)b));
}

static void
print_alignments_helper(struct fpo *fpo, bool revcmpl)
{
	static bool first = true;

	struct sw_full_results sfr;
	struct sequence *read, *contig;
	char *dbalign, *qralign;
	uint32_t genome_start, genome_len;

	for (; fpo != NULL; fpo = fpo->next) {
		if (dynhash_find(read_list, fpo->input.read, NULL,
		    (void *)&read) == false) {
			fprintf(stderr, "error: read [%s] is missing\n",
			    fpo->input.read);
			exit(1);
		}

		if (dynhash_find(contig_list, fpo->input.genome, NULL,
		    (void *)&contig) == false) {
			fprintf(stderr, "error: contig [%s] is missing\n",
			    fpo->input.genome);
			exit(1);
		}

		if (contig->revcmpl != revcmpl) {
			reverse_complement(contig->sequence, NULL,
			    contig->sequence_len);
			contig->revcmpl = revcmpl;
		}

		genome_start = fpo->input.genome_start;
		genome_len = fpo->input.genome_end - genome_start + 1;

		/* offset is given relative to the positive strand */
		if (revcmpl) {
			genome_start =
			    contig->sequence_len - fpo->input.genome_end - 1;
		}

		if (use_colours) {
			sw_full_cs(contig->sequence, genome_start, genome_len,
			    read->sequence, read->sequence_len, read->initbp,
			    fpo->input.score, &dbalign, &qralign, &sfr);
		} else {
			sw_full_ls(contig->sequence, genome_start, genome_len,
			    read->sequence, read->sequence_len,
			    fpo->input.score, fpo->input.score, &dbalign,
			    &qralign, &sfr);
		}

		if (sfr.score != fpo->input.score) {
			fprintf(stderr, "warning: score differs from input "
			    "file (read=\"%s\", genome=\"%s\")\n",
			    fpo->input.read, fpo->input.genome);
			fprintf(stderr, "         Are you using different S-W "
			    "parameters than before?\n");
		}

		if (!first)
			fputc('\n', stdout);
		first = false;

		output_normal(stdout, read->name, contig->name, &sfr,
		    contig->sequence_len, genome_start, use_colours,
		    read->sequence, read->sequence_len, read->initbp, revcmpl,
		    Rflag);

		fputs("\n\n", stdout);

		output_pretty(stdout, read->name, contig->name, &sfr, dbalign,
		    qralign, contig->sequence, contig->sequence_len,
		    genome_start, use_colours, read->sequence,
		    read->sequence_len, read->initbp, revcmpl); 
	}
}

/*
 * Run through our linked list, doing S-W on the appropriate contig
 * and read and pretty print to stdout.
 */
static void
print_alignments()
{
	int ret;

	if (use_colours) {
		ret = sw_full_cs_setup(longest_genome_len, longest_read_len,
		    gap_open, gap_extend, match_value, mismatch_value,
		    xover_penalty, false);
	} else {
		ret = sw_full_ls_setup(longest_genome_len, longest_read_len,
		    gap_open, gap_extend, match_value, mismatch_value, false);
	}
	if (ret) {
		fprintf(stderr, "failed to initialise scalar Smith-Waterman "
		    "(%s)\n", strerror(errno));
		exit(1);
	}

	/* handle forwards first */
	print_alignments_helper(alignments, false);

	/* now reverse complements */
	print_alignments_helper(alignments_revcmpl, true);
}

static void
usage(char *progname)
{
	char *slash;

	slash = strrchr(progname, '/');
	if (slash != NULL)
		progname = slash + 1;

	fprintf(stderr, "usage: %s [parameters] [options] shrimp_output_file "
	    "genome_dir reads_dir\n", progname);

	fprintf(stderr, "Parameters:\n");

	fprintf(stderr,
	    "    -m    S-W Match Value                         (default: %d)\n",
	    DEF_MATCH_VALUE);

	fprintf(stderr,
	    "    -i    S-W Mismatch Value                      (default: %d)\n",
	    DEF_MISMATCH_VALUE);

	fprintf(stderr,
	    "    -g    S-W Gap Open Penalty                    (default: %d)\n",
	    DEF_GAP_OPEN);

	fprintf(stderr,
	    "    -e    S-W Gap Extend Penalty                  (default: %d)\n",
	    DEF_GAP_EXTEND);

	if (use_colours) {
		fprintf(stderr,
		    "    -x    S-W Crossover Penalty                   ("
		    "default: %d)\n", DEF_XOVER_PENALTY);
	}

	fprintf(stderr, "\nOptions:\n");

	fprintf(stderr,
	    "    -R    Print Reads in Output (if in input)     (default: "
	    "disabled)\n");

	exit(1);
}

int
main(int argc, char **argv)
{
	char *fpout, *readsdir, *genomedir, *progname, *optstr;
	int ch;

	fprintf(stderr, "--------------------------------------------------"
	    "------------------------------\n");
	fprintf(stderr, "prettyprint: %s SPACE. (SHRiMP %s [%s])\n",
	    (use_colours) ? "COLOUR" : "LETTER", SHRIMP_VERSION_STRING,
	    get_compiler());
	fprintf(stderr, "--------------------------------------------------"
	    "------------------------------\n");

	progname = argv[0];

	if (use_colours)
		optstr = "m:i:g:e:x:R";
	else
		optstr = "m:i:g:e:R";

	while ((ch = getopt(argc, argv, optstr)) != -1) {
		switch (ch) {
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
		case 'x':
			assert(use_colours);
			xover_penalty = atoi(optarg);
			break;
		case 'R':
			Rflag = true;
			break;
		default:
			usage(progname);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 3)
		usage(progname);

	fprintf(stderr, "Settings:\n");
	fprintf(stderr, "    S-W Match Value:            %d\n", match_value);
	fprintf(stderr, "    S-W Mismatch Value:         %d\n", mismatch_value);
	fprintf(stderr, "    S-W Gap Open Penalty:       %d\n", gap_open);
	fprintf(stderr, "    S-W Gap Extend Penalty:     %d\n", gap_extend);
	
	if (use_colours) {
		fprintf(stderr, "    S-W Crossover Penalty:      %d\n",
		    xover_penalty);
	}
	fputc('\n', stderr);

	read_list   = dynhash_create(keyhasher, keycomparer);
	contig_list = dynhash_create(keyhasher, keycomparer);
	if (read_list == NULL || contig_list == NULL) {
		fprintf(stderr, "error: failed to allocate read and contig "
		    "lists\n");
		exit(1);
	}

	fpout     = argv[0];
	genomedir = argv[1];
	readsdir  = argv[2];

	fprintf(stderr, "Loading shrimp output file...\n");
	load_output_file(fpout);
	fprintf(stderr, "Loaded %" PRIu64 " alignments from shrimp output\n",
	    nalignments + nalignments_revcmpl);

	fprintf(stderr, "Loading genome contig file(s)...\n");
	file_iterator(genomedir, load_genome_file, NULL);
	fprintf(stderr, "Loaded %u genomic contigs (%" PRIu64 " total "
	    "bases)\n", dynhash_count(contig_list), ncontig_bases);

	fprintf(stderr, "Loading read file(s)...\n");
	file_iterator(readsdir, load_reads_file, NULL);
	fprintf(stderr, "Loaded %u %s reads (%" PRIu64 " total bases)\n",
	    dynhash_count(read_list),
	    (use_colours) ? "colourspace" : "letterspace", nread_bases);

	print_alignments();

	return (0);
}
