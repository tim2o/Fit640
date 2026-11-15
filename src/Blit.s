|
| Fit640 - inner loops.
| C calling convention: arguments on the stack, d0-d1/a0-a1 are scratch.
|

    .text

|-----------------------------------------------------------------------
| void CopyRowsMove16(const void *src, void *dst,
|                     long blocksPerRow, long rows,
|                     long srcSkip, long dstSkip);
|
| Copies `rows` rows of blocksPerRow * 16 bytes using the 68040 MOVE16
| instruction. src, dst and both row strides must be multiples of 16.
| 68040 only.
|-----------------------------------------------------------------------
    .balign 2
    .globl  CopyRowsMove16
CopyRowsMove16:
    movem.l %d2-%d4,-(%sp)
    move.l  16(%sp),%a0
    move.l  20(%sp),%a1
    move.l  24(%sp),%d2             | 16-byte blocks per row
    move.l  28(%sp),%d1             | rows
    move.l  32(%sp),%d3             | src skip after each row
    move.l  36(%sp),%d4             | dst skip after each row
    subq.l  #1,%d2
    subq.l  #1,%d1
.Lrow:
    move.w  %d2,%d0
.Lblock:
    .short  0xF620, 0x9000          | move16 (%a0)+,(%a1)+
    dbra    %d0,.Lblock
    adda.l  %d3,%a0
    adda.l  %d4,%a1
    dbra    %d1,.Lrow
    movem.l (%sp)+,%d2-%d4
    rts

|-----------------------------------------------------------------------
| void ScaleRow5to4(const void *src, void *dst, long groups);
|
| 8-bit pixels. For each group of 5 source pixels writes 4, keeping
| pixels 0, 1, 3 and 4 (drops the middle one).
|-----------------------------------------------------------------------
    .balign 2
    .globl  ScaleRow5to4
ScaleRow5to4:
    move.l  4(%sp),%a0
    move.l  8(%sp),%a1
    move.l  12(%sp),%d0
    subq.l  #1,%d0
.Lgroup:
    move.l  (%a0)+,%d1              | p0 p1 p2 p3
    lsl.w   #8,%d1                  | p0 p1 p3 00
    move.b  (%a0)+,%d1              | p0 p1 p3 p4
    move.l  %d1,(%a1)+
    dbra    %d0,.Lgroup
    rts

|-----------------------------------------------------------------------
| long FirstDiffLong(const void *a, const void *b, long longs);
|
| Compares two runs of 32-bit words. Returns the index of the first one
| that differs, or `longs` if they are all equal. longs: 1..32768.
|-----------------------------------------------------------------------
    .balign 2
    .globl  FirstDiffLong
FirstDiffLong:
    move.l  4(%sp),%a0
    move.l  8(%sp),%a1
    move.l  12(%sp),%d0
    move.l  %d0,%d1
    subq.l  #1,%d1
.Lcmp:
    cmpm.l  (%a0)+,(%a1)+
    dbne    %d1,.Lcmp
    beq.s   .Lsame                  | ran out with the last pair equal
    subq.l  #1,%d0
    ext.l   %d1
    sub.l   %d1,%d0                 | longs - 1 - remaining
.Lsame:
    rts

|-----------------------------------------------------------------------
| long EqualTailLongs(const void *aEnd, const void *bEnd);
|
| Walks backwards from the two end pointers and returns how many 32-bit
| words are equal before the first difference. The caller guarantees
| there is a difference.
|-----------------------------------------------------------------------
    .balign 2
    .globl  EqualTailLongs
EqualTailLongs:
    move.l  4(%sp),%a0
    move.l  8(%sp),%a1
    moveq   #-1,%d0
.Ltail:
    addq.l  #1,%d0
    move.l  -(%a0),%d1
    cmp.l   -(%a1),%d1
    beq.s   .Ltail
    rts

|-----------------------------------------------------------------------
| void ScaleRow8to7(const void *src, void *dst, long groups);
|
| 8-bit pixels. For each group of 8 source pixels writes 7, dropping
| pixel 3. The second 4 bytes are written 3 bytes after the first, so
| they overwrite the dropped pixel. Needs a 68020 or later (long writes
| at odd addresses).
|-----------------------------------------------------------------------
    .balign 2
    .globl  ScaleRow8to7
ScaleRow8to7:
    move.l  4(%sp),%a0
    move.l  8(%sp),%a1
    move.l  12(%sp),%d0
    subq.l  #1,%d0
.Lgroup8:
    move.l  (%a0)+,(%a1)            | p0 p1 p2 p3
    addq.l  #3,%a1
    move.l  (%a0)+,(%a1)+           | p4 p5 p6 p7 over p3
    dbra    %d0,.Lgroup8
    rts
