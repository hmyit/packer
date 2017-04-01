#include "packer.h"
#include "../compressor/utils.h"
#include "../compressor/compressor.h"

// new IAT
struct 
{
	// only one IID
	IMAGE_IMPORT_DESCRIPTOR IID = {};

	// the table end by a empty IID
	IMAGE_IMPORT_DESCRIPTOR __DUMMY_IID__ = {};

	// dll name
	DEFINE_STRING(DllName, "kernel32.dll");

	// INT(IAT) end by NULL
	DWORD IAT[2 + 1];
	// two necessary functions
	struct
	{
		WORD Hint = NULL;
		DEFINE_STRING(name, "LoadLibraryA");
	}LoadLibraryA;
	struct
	{
		WORD Hint = NULL;
		DEFINE_STRING(name, "GetProcAddress");
	}GetProcAddress;
}IAT;

BYTE shell_loader[] = {
	0x60,              // PUSHAD
	0x68, 0,0,0,0,     // PUSH &peInfo
	0xE8, 0,0,0,0,     // CALL shell_main
	0x89,0x45, 0xFC,   // MOV [DWORD SS:EBP-4], EAX
	0x61,              // POPAD
	0xFF,0x65, 0xFC    // JMP NEAR[DWORD SS : EBP - 4]
};

PackResult pack(char *in, char *out, int argc = 0, char **argv = NULL)
{
	// get shell code
	BYTE *s_begin = (BYTE*)shell_main;
	BYTE *s_end = (BYTE*)shell_end;
	// skip INT3
	while (*s_begin == 0xCC)s_begin++;
	// skip INT3
	while (*(s_end - 1) == 0xCC)s_end--;
	// calculate shell_size
	size_t shell_size = s_end - s_begin;
	
	/*
	// write shell_main to file
	FILE *f_shell = fopen("shellcode.bin", "wb");
	fwrite(s_begin, shell_size, 1, f_shell);
	fclose(f_shell);
	*/

	// load file
	PE pe;
	pe.load(in);

	// try to wipe reloc
	if (!pe.wipeReloc())THROW("wipe reloc failed");


	auto &nt_header = pe.getNtHeader();
	auto &sections = pe.getSections();
	DWORD NumberOfSections = nt_header.FileHeader.NumberOfSections;
	DWORD ImageBase = nt_header.OptionalHeader.ImageBase;

	DWORD packNumberOfSections = NumberOfSections;
	std::vector<bool> skipSection(NumberOfSections);
	DWORD skipDirectory[] = { IMAGE_DIRECTORY_ENTRY_RESOURCE ,IMAGE_DIRECTORY_ENTRY_TLS };

	// fill skipSection and calculate packNumberOfSections
	for (int i = 0; i < SIZEOF(skipDirectory); i++)
	{
		DWORD rva = nt_header.OptionalHeader.DataDirectory[skipDirectory[i]].VirtualAddress;
		if (rva)
		{
			packNumberOfSections--;
			skipSection[pe.getSectionByRva(rva)] = true;
		}
	}

	// skip non-data section
	for (int i = 0; i < NumberOfSections; i++)
	{

		if (!sections[i].header.SizeOfRawData)
		{
			packNumberOfSections--;
			skipSection[i] = true;
		}
	}

	// calculate section_data_size
	size_t section_data_size = 0;
	for (int i = 0; i < sections.size(); i++)
		if (!skipSection[i])
			section_data_size += sections[i].header.SizeOfRawData;

	BYTE *section_data = new BYTE[section_data_size];
	// handle sections
	for (int i = 0; i < sections.size(); i++) if (!skipSection[i])
	{
		// copy section data to newSectionData
		memcpy(section_data, sections[i].data.get(), sections[i].header.SizeOfRawData);
		section_data += sections[i].header.SizeOfRawData;
	}
	// restore original address
	section_data -= section_data_size;
	// get compress config
	Config c_config = get_config(argc, argv);
	// compression buffer
	BYTE *c_buffer = new BYTE[getBufferSize(section_data_size)];
	// compress data
	DWORD c_size = compress(c_buffer, section_data, section_data_size, c_config.lazy_match, c_config.max_chain);
	/*
	// write data to file
	FILE *f_shell = fopen("unpac.bin", "wb");
	fwrite(section_data, section_data_size, 1, f_shell);
	fclose(f_shell);
	f_shell = fopen("pac.bin", "wb");
	fwrite(c_buffer, c_size, 1, f_shell);
	fclose(f_shell);
	*/
	// release section_data
	delete[] section_data;

	// fill NULL in tail for aligned
	*(DWORD*)(c_buffer + c_size) = 0;
	DWORD aligned_c_size = (c_size + 3) / 4 * 4;

	// calculate new_section_size
	size_t new_section_size = sizeof(PEInfo)
		+ sizeof(SectionInfo)*packNumberOfSections
		+ aligned_c_size
		+ sizeof(IAT)
		+ shell_size
		+ sizeof(shell_loader);

	// add packer section
	pe.addSection((BYTE*)PACKER_SECTION_NAME,
		new_section_size,
		new_section_size,
		IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);

	// calculate the address of each part
	PEInfo *peInfo = new(pe.getSections().rbegin()->data.get()) PEInfo;
	SectionInfo *sectionInfo = (SectionInfo*)(peInfo + 1);
	BYTE *newSectionData = (BYTE*)(sectionInfo + packNumberOfSections);

	// copy compressed data
	memcpy(newSectionData, c_buffer, aligned_c_size);
	newSectionData += aligned_c_size;

	// release c_buffer
	delete[] c_buffer;

	// fill peInfo
	peInfo->ImageBase = ImageBase;
	peInfo->AddressOfEntryPoint = nt_header.OptionalHeader.AddressOfEntryPoint;
	peInfo->IIDVirtualAddress = nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
	peInfo->NumberOfSections = packNumberOfSections;
	peInfo->UncompressSize = section_data_size;

	// handle sections
	for (int i = 0, j = 0; i < sections.size() - 1; i++)if (!skipSection[i])
	{
		// fill sectionInfo
		sectionInfo[j].VirtualAddress = sections[i].header.VirtualAddress;
		sectionInfo[j].SizeOfRawData = sections[i].header.SizeOfRawData;
		j++;

		// copy section data to newSectionData
		//memcpy(newSectionData, sections[i].data.get(), sections[i].header.SizeOfRawData);
		//newSectionData += sections[i].header.SizeOfRawData;

		// set the section be writable
		sections[i].header.Characteristics |= IMAGE_SCN_MEM_WRITE;

		// remove section raw data
		sections[i].header.Misc.VirtualSize = max(sections[i].header.Misc.VirtualSize, sections[i].header.SizeOfRawData);
		pe.compactRawData(sections[i].header.PointerToRawData, sections[i].header.SizeOfRawData);
		sections[i].header.SizeOfRawData = 0;
		sections[i].header.PointerToRawData = 0;
		sections[i].data.reset();
	}


	DWORD new_section_rva = sections.rbegin()->header.VirtualAddress;
	DWORD iat_va = new_section_rva + newSectionData - (BYTE*)peInfo;
	// fill IAT and the necessary function addresses
	IAT.IID.Name = (BYTE*)&IAT.DllName - (BYTE*)&IAT + iat_va;
	IAT.IID.FirstThunk = (BYTE*)&IAT.IAT - (BYTE*)&IAT + iat_va;
	IAT.IAT[0] = (BYTE*)&IAT.LoadLibraryA - (BYTE*)&IAT + iat_va;
	IAT.IAT[1] = (BYTE*)&IAT.GetProcAddress - (BYTE*)&IAT + iat_va;
	peInfo->LoadLibraryA = ImageBase + IAT.IID.FirstThunk;
	peInfo->GetProcAddress = ImageBase + IAT.IID.FirstThunk + sizeof(DWORD);

	// copy IAT
	memcpy(newSectionData, &IAT, sizeof(IAT));
	newSectionData += sizeof(IAT);

	// copy shell_main
	memcpy(newSectionData, s_begin, shell_size);
	newSectionData += shell_size;

	// fill peInfo address
	DWORD *shell_push_call_offset = (DWORD*)(shell_loader + 2);
	*shell_push_call_offset = ImageBase + new_section_rva;
	// fill shell_main address
	shell_push_call_offset = (DWORD*)((BYTE*)shell_push_call_offset + 5);
	*shell_push_call_offset = 0xffffffff - shell_size - 10;

	// copy shell_loader
	memcpy(newSectionData, shell_loader, sizeof(shell_loader));

	// set new ep
	DWORD new_ep = new_section_size - sizeof(shell_loader) + new_section_rva;

	PackResult result = {
		result.unpacked_sections = NumberOfSections - packNumberOfSections,
		nt_header.OptionalHeader.AddressOfEntryPoint + ImageBase,
		new_ep + ImageBase
	};

	nt_header.OptionalHeader.AddressOfEntryPoint = new_ep;

	// set IAT
	nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = iat_va;
	nt_header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IAT.IID) * 2;
	pe.save(out);

	return result;
}
