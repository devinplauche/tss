    {
        const char *mpath = getenv("PROCESSOR_MARKER");
        FILE *m;
        if (!mpath) mpath = "processor.shutdown.marker";
        m = fopen(mpath, "w");
        if (m) {
            fprintf(m, "processor_uop shutdown: rx=%llu tx=%llu err=%llu\n",
                    (unsigned long long)ctx.received,
                    (unsigned long long)ctx.published,
                    (unsigned long long)ctx.errors);
            fclose(m);
        } else {
            fprintf(stderr, "processor_uop: cannot write marker '%s'\n",
                    mpath);
        }
    }
