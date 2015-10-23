typedef struct {
    int id;
    int amount;
    int price;
} Item;

typedef struct {
    Item items[20];
} BiddingSystem;

void load_items(BiddingSystem *, char *);
