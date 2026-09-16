    {
        const char *mpath = getenv("STORAGE_MARKER");
        FILE *m;
        if (!mpath) mpath = "storage.shutdown.marker";
        m = fopen(mpath, "w");
        if (m) {
            fprintf(m, "storage_uop shutdown: rx=%llu tx=%llu err=%llu\n",
                    (unsigned long long)ctx.received,
                    (unsigned long long)ctx.published,
                    (unsigned long long)ctx.errors);
            fclose(m);
        } else {
            fprintf(stderr, "storage_uop: cannot write marker '%s'\n",
                    mpath);
        }
    }
