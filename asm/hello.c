int strlen(char const *str) {
    int len = 0;
    while (str[len]) {
        ++len;
    }
    return len;
}

int main() {
    return strlen("Hello, world!");
}
