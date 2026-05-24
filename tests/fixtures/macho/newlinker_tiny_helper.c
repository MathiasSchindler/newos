const char *helper_message(void) {
    static int counter;
    static const char first[] = "newlinker macos ok\n";
    static const char later[] = "bad\n";
    counter += 1;
    return counter == 1 ? first : later;
}