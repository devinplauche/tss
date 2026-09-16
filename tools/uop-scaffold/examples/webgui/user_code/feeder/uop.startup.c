    {
        const char *vpath = getenv("FEEDER_VECTORS");
        const char *opath = getenv("FEEDER_OUT");
        const char *dstr = getenv("FEEDER_DELAY_SEC");
        int delay_sec = 4;
        FILE *vf;
        char line[256];
        int seq, input;
        if (!vpath) vpath = "vectors.txt";
        if (!opath) opath = "feeder_results.txt";
        if (dstr && sscanf(dstr, "%d", &delay_sec) == 1 && delay_sec >= 0) {
            /* parsed */
        } else {
            delay_sec = 4;
        }
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
        /* Wait for the web GUI bridge to finish loading its parameters
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
                            if (strstr(lbuf, "webgui: ready")) {
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
                                "feeder_uop: web GUI never became ready\n");
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
           the bridge's subscriber dial completes. */
        sleep(2);
        /* One vector every <delay_sec> seconds, so a human (or a
           Playwright script) driving the GUI has time to change the
           parameters between stimuli -- this is what makes the live
           interaction observable end to end. */
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
            /* Bookkeeping BEFORE the publish: the result callback runs
               on a TSS thread and can fire while this loop is still
               publishing, so it must never observe a published vector
               that isn't counted yet. */
            ctx.inputs[seq] = input;
            ctx.n_expected++;
            if (publish_STIMULUS(&ctx, &s) != 0) {
                fprintf(stderr, "feeder_uop: publish of seq=%d failed\n",
                        seq);
                ctx.n_expected--;
                continue;
            }
            printf("feeder_uop: published stimulus seq=%d value=%d\n",
                   seq, input);
            fflush(stdout);
            if (delay_sec > 0) {
                sleep((unsigned int)delay_sec);
            }
        }
        fclose(vf);
        if (ctx.n_expected == 0) {
            fprintf(stderr, "feeder_uop: no vectors published\n");
            exit_code = 1;
            goto cleanup;
        }
        printf("feeder_uop: published %d stimulus vectors\n", ctx.n_expected);
        fflush(stdout);
        /* From here on the result callback may end the run. */
        ctx.startup_done = 1;
        /* Close the race: the final result may have arrived on the TSS
           callback thread before startup_done was set, in which case
           on_result declined to terminate. If we're already done, do it
           now instead of hanging in the main loop forever. */
        if (ctx.n_received == ctx.n_expected && ctx.n_expected > 0) {
            printf("feeder_uop: collected %d/%d results (%d mismatches)\n",
                   ctx.n_received, ctx.n_expected, ctx.mismatches);
            fflush(stdout);
            raise(SIGTERM);
        }
    }
