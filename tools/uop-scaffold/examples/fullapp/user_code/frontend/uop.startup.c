    {
        /* 1. Scripted GUI actions. Stand-in for the real GUI event loop:
           each "after_seq mult add" line is what the Apply button would
           do once the result for <after_seq> is on screen. */
        const char *apath = getenv("FRONTEND_ACTIONS");
        FILE *af;
        char line[256];
        int aseq, m, ad;
        if (!apath) apath = "gui_actions.txt";
        af = fopen(apath, "r");
        if (!af) {
            fprintf(stderr, "frontend_uop: cannot open actions file '%s'\n",
                    apath);
            exit_code = 1;
            goto cleanup;
        }
        while (fgets(line, sizeof(line), af)) {
            if (line[0] == '#' || line[0] == '\n') {
                continue;
            }
            if (sscanf(line, "%d %d %d", &aseq, &m, &ad) != 3) {
                fprintf(stderr, "frontend_uop: skipping malformed action: %s",
                        line);
                continue;
            }
            if (ctx.n_actions >= 64) {
                fprintf(stderr, "frontend_uop: too many actions\n");
                break;
            }
            ctx.actions[ctx.n_actions].after_seq = aseq;
            ctx.actions[ctx.n_actions].mult = (int32_t)m;
            ctx.actions[ctx.n_actions].add = (int32_t)ad;
            ctx.n_actions++;
        }
        fclose(af);
        printf("frontend_uop: loaded %d scripted GUI actions\n",
               ctx.n_actions);
        fflush(stdout);

        /* 2. Load persisted parameters through the storage UoP -- a
           standard interface, not direct file I/O. The request can be
           dropped if the storage side's dial hasn't finished
           (slow-joiner), so retry a few times before falling back to
           defaults. */
        sleep(2);
        {
            store_req_t req;
            int attempt, waited;
            for (attempt = 1; attempt <= 4 && !ctx.loaded; attempt++) {
                req.req_id = (int32_t)attempt;
                if (publish_STORE_REQ(&ctx, &req) != 0) {
                    fprintf(stderr, "frontend_uop: STORE_REQ publish failed\n");
                }
                for (waited = 0; waited < 4 && !ctx.loaded; waited++) {
                    sleep(1);
                }
            }
        }
        if (!ctx.loaded) {
            fprintf(stderr, "frontend_uop: storage unreachable;"
                    " using defaults mult=3 add=7\n");
            ctx.mult = 3;
            ctx.add = 7;
            printf("frontend_uop: ready (mult=3 add=7, defaults)\n");
        } else {
            printf("frontend_uop: ready (mult=%d add=%d, loaded from storage)\n",
                   (int)ctx.mult, (int)ctx.add);
        }
        fflush(stdout);
    }
