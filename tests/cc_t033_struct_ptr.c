struct node {
    int val;
    int next_idx;
};

int main(void)
{
    struct node nodes[3];
    nodes[0].val = 10; nodes[0].next_idx = 1;
    nodes[1].val = 20; nodes[1].next_idx = 2;
    nodes[2].val = 30; nodes[2].next_idx = -1;

    int sum = 0;
    int idx = 0;
    while (idx >= 0) {
        sum += nodes[idx].val;
        idx = nodes[idx].next_idx;
    }
    return sum;
}
