// Link-time only. liba.so records DT_NEEDED liblog.so; the file is never put on
// the guest's search path, so the loader must satisfy it from the virtual
// module table instead of the host filesystem.

int log_print(int value)
{
	return value;
}
