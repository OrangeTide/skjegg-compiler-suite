int main(void)
{
    int m[3][3];
    int i, j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            m[i][j] = i * 3 + j + 1;
    return m[1][2] + m[2][0];
}
