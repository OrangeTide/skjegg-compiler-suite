enum color { RED, GREEN, BLUE, YELLOW = 10, PURPLE };

int main(void)
{
    enum color c = PURPLE;
    return c + BLUE;
}
