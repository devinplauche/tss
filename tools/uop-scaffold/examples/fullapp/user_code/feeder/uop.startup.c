    {
        const char *vpath = getenv("FEEDER_VECTORS");
        const char *opath = getenv("FEEDER_OUT");
        FILE *vf;
        char line[256];
        int seq, input;
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
        /* Wait for the frontend to finish loading its parameters
           (startup -> storage round trip) before publishing: no stimulus
           may be transformed with uninitialized state. The orchestrator
           passes FRONTEND_LOG; without it, fall back to a fixed delay. */
        {
            const char *flog = getenv("FRONTEND_LOG");
            if (flog) {
                int waited = 0;
                for (;;) {
                    FILE *lf = fopen(flog, "r");
                    if (lf) {
                        char lbuf[256];
                        int ready = 0;
                        while (fgets(lbuf, sizeof(lbuf), lf)) {
                            if (strstr(lbuf, "frontend_uop: ready")) {
                                ready = 1;
                                break;
                            }
                        }
                        fclose(lf);
                        if (ready) {
                            break;
                        }
                    }
                    if (++waited >= 30) {
                        fprintf(stderr,
                                "feeder_uop: frontend never became ready\n");
                        exit_code = 1;
                        goto cleanup;
                    }
                    sleep(1);
                }
            } else {
                sleep(5);
            }
        }
        /* Slow-joiner guard: nng pub/sub drops anything published before
           the frontend's subscriber dial completes. */
        sleep(2);
        while (fgets(line, sizeof(line), vf)) {
            stimulus_t s;
            if (line[0] == '#' || line[0] == '\n') {
                continue;
            }
            if (sscanf(line, "%d %d", &seq, &input) != 2) {
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
            ctx.inputs[seq] = input;
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
