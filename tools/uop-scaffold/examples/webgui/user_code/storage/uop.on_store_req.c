    {
        /* External-storage read. In deployment this backing file lives on
           network storage; the frontend never touches it directly --
           every access arrives here as a UoP message. Missing file means
           no state was ever saved: answer with the defaults. */
        const char *path = getenv("STORE_FILE");
        FILE *f;
        store_state_t s;
        int m, ad;
        s.mult = 3;
        s.add = 7;
        if (!path) path = "app_state.txt";
        f = fopen(path, "r");
        if (f) {
            char line[256];
            /* First data line wins; '#' starts a comment, like the other
               demo readers. */
            while (fgets(line, sizeof(line), f)) {
                if (line[0] == '#' || line[0] == '\n') {
                    continue;
                }
                if (sscanf(line, "%d %d", &m, &ad) == 2) {
                    s.mult = (int32_t)m;
                    s.add = (int32_t)ad;
                }
                break;
            }
            fclose(f);
        }
        if (publish_STORE_RESP(ctx, &s) != 0) {
            fprintf(stderr, "storage_uop: STORE_RESP publish failed\n");
        }
    }
    (void)msg;
