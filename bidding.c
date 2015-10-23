#include "bidding.h"
#include <stdio.h>

void load_items(BiddingSystem *b, char *filename) {
    FILE *fp;
    fp = fopen(filename, "rb");
    fread(b->items, sizeof(Item), 20, fp);
}

