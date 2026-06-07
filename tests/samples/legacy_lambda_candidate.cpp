bool isPalindrome(long long x)
{
    long long original{x};
    long long temp{0};
    while (x > 0)
    {
        if (x < 0)
        {
            return false;
        }
        temp = temp * 10 + x % 10;
        x /= 10;
    }
    return original == temp;
}
