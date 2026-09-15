# The data, and what it cannot tell you

Back to the [README](../README.md).

## The data, and what it cannot tell you

Hyperliquid ETH-perp, collected live, 2026-08-07 to 2026-09-14.

| | |
|---|---|
| book | top 20 levels of each side, one snapshot every 5 seconds, JSONL.gz |
| trades | every print, deduplicated on trade id, venue millisecond timestamps |
| snapshots present | 639,307 of 673,920 expected, 94.86% |
| tick size | 0.1 USD; the price ran 1853.1 to 2663.0 over the period, median mid 2443.4 |
| spread | one tick in 98.39% of snapshots over the whole period, 99.46% over the holdout |
| daily volume | about 37,000 ETH on the calibration days |
| prints | 1,009,459 |

Both records carry two timestamps: `time`, the venue's own clock, and `rx`, when
the collector received the message. Everything downstream orders on `time`. On
2026-08-09 the median difference between them was 366 ms and the 99th percentile
667 ms.

Days that are not complete, and how they are handled:

| day | coverage | treatment |
|---|---|---|
| 2026-08-07 | 9.59% | excluded everywhere |
| 2026-08-08 | 99.85% | before the calibration window, unused |
| 2026-08-15 | 99.85% | excluded from the holdout by the pre-registration, reported as a sensitivity |
| 2026-08-19 | 31.05% | excluded from the extension set |
| 2026-09-14 | 62.27% | no trade file at all, excluded from the extension set |

The thing that matters most about this data is that **the book is a snapshot
every five seconds, not an order by order feed.** Individual orders, their ids,
their arrival times and their queue positions are not observable. An order added
and cancelled inside one window never existed as far as this repository is
concerned. The only millisecond resolution data here is the trade tape. Every
place where that limit changes what a number means, it is said again.
