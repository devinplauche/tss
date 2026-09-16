    int inputs[256];   /* stimulus input per seq (1..255), from vectors file */
    int n_expected;    /* stimulus vectors published */
    int n_received;    /* results collected */
    int mismatches;    /* results != recomputed expectation */
    FILE *out;         /* results log; NULL once closed */
    int startup_done;  /* set at the end of startup: only then may the
                          result callback end the run. Results can arrive
                          while startup is still publishing (slow publish
                          for live GUI interaction), so the completion
                          check must not fire mid-startup. */
