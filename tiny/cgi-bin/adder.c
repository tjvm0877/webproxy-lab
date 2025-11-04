/* adder.c - a minimal CGI program that adds two numbers together */
#include <stdlib.h>
#include "csapp.h"

int main(void)
{
    const char *qs = getenv("QUERY_STRING");
    int a = 0, b = 0;

    printf("Content-type: text/html\r\n\r\n");
    if (!qs || sscanf(qs, "%d&%d", &a, &b) != 2)
    {
        printf("<html><body>Invalid query</body></html>\n");
        exit(0);
    }

    printf("<html><body>sum=%d</body></html>\n", a + b);
    exit(0);
}
