extern int printk(char *, ...) __attribute__((format (printf, 1, 2)));

void module_init(void)
{
    printk("Hello from a dynamically loaded module!\n");
}

void module_exit(void)
{
    printk("Module unloaded!\n");
}
