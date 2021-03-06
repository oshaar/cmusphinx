/*
 * gausubvq.c -- Sub-vector cluster Gaussian densities
 * 
 * **********************************************
 * CMU ARPA Speech Project
 *
 * Copyright (c) 1999 Carnegie Mellon University.
 * ALL RIGHTS RESERVED.
 * **********************************************
 * 
 * HISTORY
 * 
 * 09-Mar-1999	M K Ravishankar (rkm@cs.cmu.edu) at Carnegie Mellon University
 * 		Started.
 */


#include <libutil/libutil.h>
#include <libmain/gauden.h>


static int32 **parse_subvecs (char *str)
{
    char *strp;
    int32 n, n2, l;
    glist_t dimlist;	/* List of dimensions in one subvector */
    glist_t veclist;	/* List of dimlists (subvectors) */
    int32 **subvec;
    gnode_t *gn, *gn2;
    
    veclist = NULL;
    
    for (strp = str; *strp; ) {
	dimlist = NULL;
	
       	for (;;) {
	    if (sscanf (strp, "%d%n", &n, &l) != 1)
		E_FATAL ("'%s': Couldn't read int32 @pos %d\n", str, strp-str);
	    strp += l;

	    if (*strp == '-') {
		strp++;
		
		if (sscanf (strp, "%d%n", &n2, &l) != 1)
		    E_FATAL ("'%s': Couldn't read int32 @pos %d\n", str, strp-str);
		strp += l;
	    } else
		n2 = n;
	    
	    if ((n < 0) || (n > n2))
		E_FATAL("'%s': Bad subrange spec ending @pos %d\n", str, strp-str);
	    
	    for (; n <= n2; n++) {
		if (glist_chkdup_int32 (dimlist, n))
		    E_FATAL("'%s': Duplicate dimension ending @pos %d\n", str, strp-str);
		
		dimlist = glist_add_int32 (dimlist, n);
	    }
	    
	    if ((*strp == '\0') || (*strp == '/'))
		break;
	    
	    if (*strp != ',')
		E_FATAL("'%s': Bad delimiter @pos %d\n", str, strp-str);
	    
	    strp++;
	}
	
	veclist = glist_add_ptr (veclist, dimlist);
	
	if (*strp == '/')
	    strp++;
    }

    /* Convert the glists to arrays; remember that the glists are in reverse order!! */
    n = glist_count (veclist);					/* #Subvectors */
    subvec = (int32 **) ckd_calloc (n+1, sizeof(int32 *));	/* +1 for sentinel */
    
    subvec[n] = NULL;
    for (--n, gn = veclist; (n >= 0) && gn; gn = gnode_next(gn), --n) {
	gn2 = (glist_t) gnode_ptr (gn);
	
	n2 = glist_count (gn2);					/* Length of this subvector */
	if (n2 <= 0)
	    E_FATAL("'%s': 0-length subvector\n", str);
	
	subvec[n] = (int32 *) ckd_calloc (n2+1, sizeof(int32));	/* +1 for sentinel */
	subvec[n][n2] = -1;
	
	for (--n2; (n2 >= 0) && gn2; gn2 = gnode_next(gn2), --n2)
	    subvec[n][n2] = gnode_int32 (gn2);
	assert ((n2 < 0) && (! gn2));
    }
    assert ((n < 0) && (! gn));
    
    /* Free the glists */
    for (gn = veclist; gn; gn = gnode_next(gn)) {
	gn2 = (glist_t) gnode_ptr(gn);
	glist_free (gn2);
    }
    glist_free (veclist);
    
    return subvec;
}


static void usagemsg (char *pgm)
{
    fprintf (stderr, "Usage: %s \\\n", pgm);
    fprintf (stderr, "\tmeanfile \\\n");
    fprintf (stderr, "\tvarfile \\\n");
    fprintf (stderr, "\tsubvecs (e.g., 24,0-11/25,12-23/26,27-38) \\\n");
    fprintf (stderr, "\tVQsize (#vectors) \\\n");
    fprintf (stderr, "\teps (e.g. 0.001; stop if relative decrease in sqerror < epsilon \\\n");
    fprintf (stderr, "\ttrials (#trials with different random initializations for each VQ \\\n");
    fprintf (stderr, "\toutfile \\\n");
    fprintf (stderr, "\t[skipvar]\n");
    fflush (stderr);
    
    exit(0);
}


main (int32 argc, char *argv[])
{
    gauden_t *g;
    int32 **subvec;
    int32 i, j, n, m, c, s;
    int32 svsize, maxiter, maxtrial, trial;
    float32 **data, **vqmean, **b_vqmean;
    int32 *datamap, *vqmap, *b_vqmap;
    int32 vqsize, max_invec, skipvar;
    float32 epsilon;
    float64 sqerr, b_sqerr;
    FILE *fp;
    
    if ((argc != 8) && (argc != 9))
	usagemsg (argv[0]);
    
    maxiter = 100;	/* Default */
    
    if ((sscanf (argv[4], "%d", &vqsize) != 1) || (vqsize <= 0))
	E_FATAL("Bad VQsize argument: '%s'\n", argv[4]);
    
    if ((sscanf (argv[5], "%f", &epsilon) != 1) || (epsilon <= 0.0))
	E_FATAL("Bad epsilon argument: '%s'\n", argv[5]);
    
    if ((sscanf (argv[6], "%d", &maxtrial) != 1) || (maxtrial <= 0))
	E_FATAL("Bad trials argument: '%s'\n", argv[6]);
    
    skipvar = 0;
    if (argc > 8) {
	if (strcmp (argv[8], "skipvar") == 0)
	    skipvar = 1;
    }
    
    if ((fp = fopen(argv[7], "w")) == NULL)
	E_FATAL("fopen(%s,w) failed\n", argv[7]);
    
    for (i = 0; i < argc-1; i++)
	fprintf (fp, "# %s \\\n", argv[i]);
    fprintf (fp, "# %s\n#\n", argv[argc-1]);
    
    logs3_init ((float64) 1.0003);
    
    /* Load means/vars but DO NOT precompute variance inverses or determinants */
    g = gauden_init (argv[1], argv[2], 0.0 /* no varfloor */, FALSE);
    assert (g->n_feat == 1);
    gauden_var_nzvec_floor (g, 0.0001);
    
    /* Parse subvector spec argument; subvec is null terminated; subvec[x] is -1 terminated */
    subvec = parse_subvecs (argv[3]);
    
    /* Print header */
    for (s = 0; subvec[s]; s++);
    fprintf (fp, "VQParam %d %d -> %d %d\n", gauden_n_mgau(g), gauden_max_n_mean(g), s, vqsize);
    
    for (s = 0; subvec[s]; s++) {
	for (i = 0; subvec[s][i] >= 0; i++);
	fprintf (fp, "Subvector %d length %d ", s, i);
	for (i = 0; subvec[s][i] >= 0; i++)
	    fprintf (fp, " %2d", subvec[s][i]);
	fprintf (fp, "\n");
    }
    fflush (fp);
    
    /*
     * datamap[] for identifying non-0 input vectors that take part in the clustering process:
     *     datamap[m*max_mean + c] = row index of data[][] containing the copy.
     * vqmap[] for mapping vq input data to vq output.
     */
    max_invec = gauden_n_mgau(g) * gauden_max_n_mean(g);
    datamap = (int32 *) ckd_calloc (max_invec, sizeof(int32));
    vqmap = (int32 *) ckd_calloc (max_invec, sizeof(int32));
    b_vqmap = (int32 *) ckd_calloc (max_invec, sizeof(int32));
    
    /* Copy and cluster each subvector */
    for (s = 0; subvec[s]; s++) {
	E_INFO("Clustering subvector %d\n", s);
	
	/* Subvector length */
	for (svsize = 0; subvec[s][svsize] >= 0; svsize++);
	
	/* Allocate input/output data areas and initialize maps */
	data = (float32 **) ckd_calloc_2d (max_invec, svsize*2, sizeof(float32));
	vqmean = (float32 **) ckd_calloc_2d (vqsize, svsize*2, sizeof(float32));
	b_vqmean = (float32 **) ckd_calloc_2d (vqsize, svsize*2, sizeof(float32));
	for (i = 0; i < max_invec; i++)
	    datamap[i] = -1;
	
	/* Make a copy of the subvectors from the input data */
	n = 0;
	for (m = 0; m < gauden_n_mgau(g); m++) {	/* For each codebook */
	    assert (gauden_n_mean(g, m, 0) == gauden_n_var(g, m, 0));
	    
	    for (c = 0; c < gauden_n_mean(g, m, 0); c++) {	/* For each codeword */
		if (vector_is_zero (g->mgau[m][0].var[c], g->featlen[0]))
		    continue;
		
		for (i = 0; i < svsize; i++) {	/* Copy the selected dimensions, mean and var */
		    data[n][i*2] = g->mgau[m][0].mean[c][subvec[s][i]];
		    data[n][i*2 + 1] = g->mgau[m][0].var[c][subvec[s][i]];
		}
		datamap[m * gauden_max_n_mean(g) + c] = n;
		
		n++;
	    }
	}
	
	/*
	 * Make several VQ trials; each should be different since vector_vqgen begins with a
	 * different random initialization each time.
	 */
	for (trial = 0; trial < maxtrial; trial++) {
	    E_INFO("Trial %d\n", trial);
	    
	    E_INFO("Sanity check: data[0]:\n");
	    vector_print (stdout, data[0], svsize*2);
	    
	    for (i = 0; i < max_invec; i++)
		vqmap[i] = -1;
#if 0
	    {
		int32 **in;
		
		printf ("Input data: %d x %d\n", n, svsize*2);
		in = (int32 **)data;
		for (i = 0; i < n; i++) {
		    printf ("%8d:", i);
		    for (j = 0; j < svsize*2; j++)
			printf (" %08x", in[i][j]);
		    printf ("\n");
		}
		for (i = 0; i < n; i++) {
		    printf ("%15d:", i);
		    for (j = 0; j < svsize*2; j++)
			printf (" %15.7e", data[i][j]);
		    printf ("\n");
		}
		fflush (stdout);
	    }
#endif
	    /* VQ the subvector copy built above */
	    if (skipvar)
		sqerr = vector_vqgen2 (data, n, svsize*2, vqsize, epsilon, maxiter,
				       vqmean, vqmap);
	    else
		sqerr = vector_vqgen (data, n, svsize*2, vqsize, epsilon, maxiter,
				      vqmean, vqmap);
	    
	    if ((trial == 0) || (sqerr < b_sqerr)) {
		b_sqerr = sqerr;
		memcpy (b_vqmap, vqmap, max_invec * sizeof(int32));
		for (i = 0; i < vqsize; i++)
		    memcpy (b_vqmean[i], vqmean[i], svsize*2 * sizeof(float32));
	    }
	}
	
	/* Output VQ */
	fprintf (fp, "Codebook %d Sqerr %e\n", s, b_sqerr);
	for (m = 0; m < vqsize; m++)
	    vector_print (fp, b_vqmean[m], svsize*2);
	
	fprintf (fp, "Map %d\n", s);
	for (i = 0; i < max_invec; i += gauden_max_n_mean(g)) {
	    for (j = 0; j < gauden_max_n_mean(g); j++) {
		if (datamap[i+j] < 0)
		    fprintf (fp, " -1");
		else
		    fprintf (fp, " %d", b_vqmap[datamap[i+j]]);
	    }
	    fprintf (fp, "\n");
	}
	fflush (fp);
	
	/* Cleanup */
	ckd_free_2d ((void **) data);
	ckd_free_2d ((void **) vqmean);
	ckd_free_2d ((void **) b_vqmean);
    }
    
    fprintf (fp, "End\n");
    fclose (fp);
    
    exit(0);
}
