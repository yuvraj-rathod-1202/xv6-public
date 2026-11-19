# xv6 scheduler test results

25/25 checks passed in 17.1 minutes.

| Check | Result | Time (s) | Detail |
|---|---|---|---|
| rbtree unit tests, 1.8M verified operations (host) | PASS | 2 |  |
| rbtree mutation test: every broken tree is caught (host) | PASS | 10 |  |
| build debug kernel | PASS | 4 |  |
| debug 1cpu: schedtest | PASS | 9 |  |
| debug 1cpu: schedstress | PASS | 5 |  |
| debug 2cpu: schedtest | PASS | 8 |  |
| debug 2cpu: schedstress | PASS | 5 |  |
| debug 4cpu: schedtest | PASS | 9 |  |
| debug 4cpu: schedstress | PASS | 5 |  |
| debug 4cpu long: schedstress 4000 ticks | PASS | 40 |  |
| usertests rr 2cpu: set rr | PASS | 0 |  |
| usertests rr 2cpu: usertests | PASS | 55 |  |
| usertests cfs 4cpu: set cfs | PASS | 0 |  |
| usertests cfs 4cpu: usertests | PASS | 246 |  |
| build release kernel | PASS | 4 |  |
| release 2cpu: schedtest | PASS | 8 |  |
| bench 1cpu: schedbench | PASS | 3 |  |
| bench 1cpu: schedoverhead | PASS | 604 |  |
| trace 1cpu: trace rr | PASS | 2 |  |
| trace 1cpu: trace fifo | PASS | 0 |  |
| trace 1cpu: trace cfs | PASS | 1 |  |
| trace 1cpu: trace ascii cfs | PASS | 0 |  |
| gantt chart rr | PASS | 0 | results/gantt-rr.svg |
| gantt chart fifo | PASS | 0 | results/gantt-fifo.svg |
| gantt chart cfs | PASS | 0 | results/gantt-cfs.svg |
