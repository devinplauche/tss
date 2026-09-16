    int expected[256]; /* expected output per seq (1..255), from vectors file */
    int n_expected;      /* stimulus vectors published */
    int n_received;      /* results collected */
    int mismatches;      /* results != expected */
    FILE *out;           /* results log; NULL once closed */
