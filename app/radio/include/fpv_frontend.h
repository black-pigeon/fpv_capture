#ifndef FPV_FRONTEND_H
#define FPV_FRONTEND_H
#include "fpv_board.h"
struct fpv_frontend { const struct fpv_board *board; int fd, port, valid; };
int fpv_frontend_open(struct fpv_frontend *front,const struct fpv_board *board,uint64_t hz);
int fpv_frontend_select(struct fpv_frontend *front,uint64_t hz);
void fpv_frontend_close(struct fpv_frontend *front);
#endif
