"""Playwright end-to-end tests for the web GUI front end.

These drive a real browser against the bridge's GUI while the full TSS
fabric runs underneath:

    feeder_uop (C) --TSS--> webgui bridge --HTTP--> this browser
                        <--TSS--              (Apply button)

The tests assume the demo stack is already up (``demo.sh`` starts the
storage UoP, the bridge, and the slow-publishing feeder, then runs this
file). Configuration comes from the environment:

    GUI_URL      e.g. http://127.0.0.1:18090/ (default)
    CHROME_PATH  browser binary (default /opt/meta-chromium/chrome)

Scenario: the seed state (mult=7, add=3) loads from storage and shows in
the GUI; stimuli flow through with those parameters; the test clicks
Apply twice (5/1, then 9/-3); each time the very next transformed
results echo the new parameters -- proving the GUI action travelled
browser -> bridge RAM -> TSS RESULT -> back around the loop.
"""

import os
import re

import pytest
from playwright.sync_api import Browser, Page, expect, sync_playwright

GUI_URL = os.environ.get("GUI_URL", "http://127.0.0.1:18090/")
CHROME_PATH = os.environ.get("CHROME_PATH", "/opt/meta-chromium/chrome")
RESULT_TIMEOUT_MS = 30_000


@pytest.fixture(scope="module")
def browser():
    with sync_playwright() as p:
        b = p.chromium.launch(
            executable_path=CHROME_PATH,
            args=["--no-sandbox", "--disable-dev-shm-usage",
                  # Chromium >= 142 blocks public-context pages from
                  # reaching localhost (Local Network Access checks);
                  # the GUI is served on 127.0.0.1, so opt out.
                  "--disable-features=LocalNetworkAccessChecks"],
        )
        yield b
        b.close()


@pytest.fixture(scope="module")
def page(browser: Browser):
    pg = browser.new_page()
    pg.goto(GUI_URL)
    yield pg
    pg.close()


def _apply(page: Page, mult: int, add: int):
    page.locator("#in-mult").fill(str(mult))
    page.locator("#in-add").fill(str(add))
    page.locator("#apply").click()


def test_gui_shows_state_loaded_from_storage(page: Page):
    """Seed state (7/3) arrives via STORE_REQ/STORE_RESP and renders."""
    expect(page.locator("#state-mult")).to_have_text("7")
    expect(page.locator("#state-add")).to_have_text("3")
    expect(page.locator("#state-source")).to_have_text("storage")
    expect(page.locator("#log")).to_contain_text("loaded from storage")


def test_pipeline_live_with_seed_params(page: Page):
    """A stimulus round-trips through the untouched GUI state first."""
    expect(page.locator("#log")).to_contain_text(
        re.compile(r"RESULT seq=\d+ value=-?\d+ \(mult=7 add=3\)"),
        timeout=RESULT_TIMEOUT_MS,
    )


def test_apply_updates_live_params(page: Page):
    """Clicking Apply mutates the bridge's live RAM state."""
    _apply(page, 5, 1)
    expect(page.locator("#state-mult")).to_have_text("5")
    expect(page.locator("#state-add")).to_have_text("1")
    expect(page.locator("#log")).to_contain_text("GUI Apply -> mult=5 add=1")


def test_result_uses_gui_params(page: Page):
    """The next transformed result echoes the GUI-set parameters."""
    expect(page.locator("#log")).to_contain_text(
        re.compile(r"RESULT seq=\d+ value=-?\d+ \(mult=5 add=1\)"),
        timeout=RESULT_TIMEOUT_MS,
    )


def test_second_apply_flows_back(page: Page):
    """A second Apply (9/-3) is picked up by subsequent transforms."""
    _apply(page, 9, -3)
    expect(page.locator("#state-mult")).to_have_text("9")
    expect(page.locator("#log")).to_contain_text(
        re.compile(r"RESULT seq=\d+ value=-?\d+ \(mult=9 add=-3\)"),
        timeout=RESULT_TIMEOUT_MS,
    )
