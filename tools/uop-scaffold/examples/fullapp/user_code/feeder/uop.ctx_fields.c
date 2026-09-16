    int inputs[256];   /* stimulus input per seq (1..255), from vectors file */
    int n_expected;    /* stimulus vectors published */
    int n_received;    /* results collected */
    int mismatches;    /* results != recomputed expectation */
    FILE *out;         /* results log; NULL once closed */
