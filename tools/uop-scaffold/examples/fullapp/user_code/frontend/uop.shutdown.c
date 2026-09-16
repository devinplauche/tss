    {
        /* Offload RAM state to external storage through the standard
           interface. The main loop has exited but the connections are
           still open, so the PUT still goes out. */
        store_state_t s;
        const char *mpath = getenv("FRONTEND_MARKER");
        FILE *m;
        s.mult = ctx.mult;
        s.add = ctx.add;
        if (publish_STORE_PUT(&ctx, &s) != 0) {
            fprintf(stderr, "frontend_uop: STORE_PUT publish failed\n");
        } else {
            printf("frontend_uop: offloaded state mult=%d add=%d to storage\n",
                   (int)ctx.mult, (int)ctx.add);
            fflush(stdout);
        }
        /* Grace period so the PUT flushes before teardown. */
        sleep(1);
        if (!mpath) mpath = "frontend.shutdown.marker";
        m = fopen(mpath, "w");
        if (m) {
            fprintf(m, "frontend_uop shutdown: rx=%llu tx=%llu err=%llu\n",
                    (unsigned long long)ctx.received,
                    (unsigned long long)ctx.published,
                    (unsigned long long)ctx.errors);
            fclose(m);
        } else {
            fprintf(stderr, "frontend_uop: cannot write marker '%s'\n",
                    mpath);
        }
    }
