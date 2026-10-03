package dev.rehan.passthrough.client;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Locale;

/**
 * Memory both games see.
 * <ul>
 *   <li>Windows: a named, pagefile-backed Win32 file mapping; other processes open it by name.</li>
 *   <li>Linux (Minecraft native, the host game under Wine/Proton): a file in /dev/shm (tmpfs, never touches the
 *   disk). A Wine process opens the same file as {@code Z:\dev\shm\<name>} and maps it with CreateFileMapping, so
 *   both sides share the same pages. A Wine named mapping would be invisible to a native JVM.</li>
 * </ul>
 */
final class SharedMemory {
	private static final int PAGE_READWRITE = 0x04;
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;
	static final boolean WINDOWS = System.getProperty("os.name", "").toLowerCase(Locale.ROOT).startsWith("windows");
	final MemorySegment segment;
	/** Where the other side finds it: the mapping name on Windows, the file path on Linux. */
	final String location;

	private SharedMemory(final MemorySegment segment, final String location) {
		this.segment = segment;
		this.location = location;
	}

	/** {@code name} is the Win32 mapping name; on Linux its last path component names the /dev/shm file. */
	static SharedMemory create(final String name, final long size) throws Throwable {
		return WINDOWS ? createWin32(name, size) : createFile(Path.of("/dev/shm", fileName(name)), size);
	}

	static String fileName(final String name) {
		return name.substring(name.lastIndexOf('\\') + 1);
	}

	private static SharedMemory createFile(final Path path, final long size) throws IOException {
		try (FileChannel ch = FileChannel.open(path, StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE)) {
			// sparse on tmpfs: pages are only allocated as they are written
			if (ch.size() != size) {
				ch.truncate(0);
				ch.position(size - 1);
				ch.write(java.nio.ByteBuffer.wrap(new byte[]{0}));
			}

			// the mapping outlives the channel
			MemorySegment view = ch.map(FileChannel.MapMode.READ_WRITE, 0, size, Arena.global());
			return new SharedMemory(view, path.toString());
		}
	}

	private static SharedMemory createWin32(final String name, final long size) throws Throwable {
		Linker linker = Linker.nativeLinker();
		SymbolLookup kernel32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		MethodHandle createFileMapping = linker.downcallHandle(
			kernel32.find("CreateFileMappingW").orElseThrow(),
			FunctionDescriptor.of(
				ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.ADDRESS
			)
		);
		MethodHandle mapViewOfFile = linker.downcallHandle(
			kernel32.find("MapViewOfFile").orElseThrow(),
			FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.JAVA_LONG)
		);
		MemorySegment wideName = Arena.global().allocateFrom(ValueLayout.JAVA_BYTE, (name + "\0").getBytes(StandardCharsets.UTF_16LE));
		MemorySegment invalidHandle = MemorySegment.ofAddress(-1L);
		MemorySegment handle = (MemorySegment)createFileMapping.invoke(
			invalidHandle, MemorySegment.NULL, PAGE_READWRITE, (int)(size >>> 32), (int)size, wideName
		);
		if (handle.address() == 0L) {
			throw new IllegalStateException("CreateFileMappingW failed for " + name);
		}

		MemorySegment view = (MemorySegment)mapViewOfFile.invoke(handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
		if (view.address() == 0L) {
			throw new IllegalStateException("MapViewOfFile failed for " + name);
		}

		return new SharedMemory(view.reinterpret(size), name);
	}
}
