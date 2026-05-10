int main(void)
{
    int sum = 0;
    int i = 1;
loop:
    if (i > 10) goto done;
    sum += i;
    i++;
    goto loop;
done:
    return sum;
}
