    {
        /* External-storage write: persist the frontend's offloaded state.
           The write is atomic enough for this demo (single writer); a
           production backend would fsync / journal here. */
        const char *path = getenv("STORE_FILE");
        FILE *f;
        if (!path) path = "app_state.txt";
        f = fopen(path, "w");
        if (!f) {
            fprintf(stderr, "storage_uop: cannot write '%s'\n", path);
            ctx->errors++;
            return;
        }
        fprintf(f, "%d %d\n", (int)msg.mult, (int)msg.add);
        fclose(f);
        printf("storage_uop: stored mult=%d add=%d\n",
               (int)msg.mult, (int)msg.add);
        fflush(stdout);
    }
