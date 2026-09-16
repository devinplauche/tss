    {
        const char *vpath = getenv("FEEDER_VECTORS");
        const char *opath = getenv("FEEDER_OUT");
        FILE *vf;
        char line[256];
        int seq, input, expected;
        if (!vpath) vpath = "vectors.txt";
        if (!opath) opath = "feeder_results.txt";
        ctx.out = fopen(opath, "w");
        if (!ctx.out) {
            fprintf(stderr, "feeder_uop: cannot open results file '%s'\n",
                    opath);
            exit_code = 1;
            goto cleanup;
        }
        vf = fopen(vpath, "r");
        if (!vf) {
            fprintf(stderr, "feeder_uop: cannot open vectors file '%s'\n",
                    vpath);
            exit_code = 1;
            goto cleanup;
        }
        /* Give the processor's subscriber time to dial in: nng pub/sub
           drops anything published before the subscriber connects
           (slow-joiner). The orchestrator already starts the processor
           first and waits for its "running" banner; this covers the TCP
           connect plus subscription propagation on top of that. */
        sleep(2);
        while (fgets(line, sizeof(line), vf)) {
            stimulus_t s;
            if (line[0] == '#' || line[0] == '\n') {
                continue;
            }
            if (sscanf(line, "%d %d %d", &seq, &input, &expected) != 3) {
                fprintf(stderr, "feeder_uop: skipping malformed line: %s",
                        line);
                continue;
            }
            if (seq < 1 || seq > 255 || ctx.n_expected >= 255) {
                fprintf(stderr, "feeder_uop: skipping bad vector (seq=%d)\n",
                        seq);
                continue;
            }
            s.seq = (int32_t)seq;
            s.value = (int32_t)input;
            if (publish_STIMULUS(&ctx, &s) != 0) {
                fprintf(stderr, "feeder_uop: publish of seq=%d failed\n",
                        seq);
                continue;
            }
            ctx.expected[seq] = expected;
            ctx.n_expected++;
        }
        fclose(vf);
        if (ctx.n_expected == 0) {
            fprintf(stderr, "feeder_uop: no vectors published\n");
            exit_code = 1;
            goto cleanup;
        }
        printf("feeder_uop: published %d stimulus vectors\n", ctx.n_expected);
        fflush(stdout);
    }
