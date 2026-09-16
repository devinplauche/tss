    {
        const char *mpath = getenv("FEEDER_MARKER");
        FILE *m;
        if (!mpath) mpath = "feeder.shutdown.marker";
        m = fopen(mpath, "w");
        if (m) {
            fprintf(m, "feeder_uop shutdown: rx=%llu tx=%llu err=%llu\n",
                    (unsigned long long)ctx.received,
                    (unsigned long long)ctx.published,
                    (unsigned long long)ctx.errors);
            fclose(m);
        } else {
            fprintf(stderr, "feeder_uop: cannot write marker '%s'\n", mpath);
        }
    }
