// Copyright Epic Games, Inc. All Rights Reserved.
// generated
#ifndef _SYMS_META_MACH_C
#define _SYMS_META_MACH_C
//~ generated from code at syms/metaprogram/syms_metaprogram_serial.c:1135
SYMS_API SYMS_Arch
syms_mach_arch_from_cputype(SYMS_MachCpuType v){
SYMS_Arch result = SYMS_Arch_Null;
switch (v){
default: break;
case SYMS_MachCpuType_X86: result = SYMS_Arch_X86; break;
case SYMS_MachCpuType_X86_64: result = SYMS_Arch_X64; break;
case SYMS_MachCpuType_ARM: result = SYMS_Arch_ARM32; break;
case SYMS_MachCpuType_ARM64: result = SYMS_Arch_ARM; break;
}
return(result);
}

//~ generated from code at syms/metaprogram/syms_metaprogram_serial.c:1591
SYMS_API void
syms_bswap_in_place__SYMS_MachLCStr(SYMS_MachLCStr *v)
{
v->offset = syms_bswap_u32(v->offset);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachUUID(SYMS_MachUUID *v)
{
v->cmd = syms_bswap_u32(v->cmd);
v->cmdsize = syms_bswap_u32(v->cmdsize);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachDylib(SYMS_MachDylib *v)
{
syms_bswap_in_place__SYMS_MachLCStr(&v->name);
v->timestamp = syms_bswap_u32(v->timestamp);
v->current_version = syms_bswap_u32(v->current_version);
v->compatability_version = syms_bswap_u32(v->compatability_version);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachDylibCommand(SYMS_MachDylibCommand *v)
{
v->cmd = syms_bswap_u32(v->cmd);
v->cmdsize = syms_bswap_u32(v->cmdsize);
syms_bswap_in_place__SYMS_MachDylib(&v->dylib);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachDyldInfoCommand(SYMS_MachDyldInfoCommand *v)
{
v->cmd = syms_bswap_u32(v->cmd);
v->cmdsize = syms_bswap_u32(v->cmdsize);
v->rebase_off = syms_bswap_u32(v->rebase_off);
v->rebase_size = syms_bswap_u32(v->rebase_size);
v->bind_off = syms_bswap_u32(v->bind_off);
v->bind_size = syms_bswap_u32(v->bind_size);
v->weak_bind_off = syms_bswap_u32(v->weak_bind_off);
v->weak_bind_size = syms_bswap_u32(v->weak_bind_size);
v->lazy_bind_off = syms_bswap_u32(v->lazy_bind_off);
v->lazy_bind_size = syms_bswap_u32(v->lazy_bind_size);
v->export_off = syms_bswap_u32(v->export_off);
v->export_size = syms_bswap_u32(v->export_size);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachLoadCommand(SYMS_MachLoadCommand *v)
{
v->type = syms_bswap_u32(v->type);
v->size = syms_bswap_u32(v->size);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachFatHeader(SYMS_MachFatHeader *v)
{
v->magic = syms_bswap_u32(v->magic);
v->nfat_arch = syms_bswap_u32(v->nfat_arch);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachFatArch(SYMS_MachFatArch *v)
{
v->cputype = syms_bswap_u32(v->cputype);
v->cpusubtype = syms_bswap_u32(v->cpusubtype);
v->offset = syms_bswap_u32(v->offset);
v->size = syms_bswap_u32(v->size);
v->align = syms_bswap_u32(v->align);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachHeader32(SYMS_MachHeader32 *v)
{
v->magic = syms_bswap_u32(v->magic);
v->cputype = syms_bswap_u32(v->cputype);
v->cpusubtype = syms_bswap_u32(v->cpusubtype);
v->filetype = syms_bswap_u32(v->filetype);
v->ncmds = syms_bswap_u32(v->ncmds);
v->sizeofcmds = syms_bswap_u32(v->sizeofcmds);
v->flags = syms_bswap_u32(v->flags);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachHeader64(SYMS_MachHeader64 *v)
{
v->magic = syms_bswap_u32(v->magic);
v->cputype = syms_bswap_u32(v->cputype);
v->cpusubtype = syms_bswap_u32(v->cpusubtype);
v->filetype = syms_bswap_u32(v->filetype);
v->ncmds = syms_bswap_u32(v->ncmds);
v->sizeofcmds = syms_bswap_u32(v->sizeofcmds);
v->flags = syms_bswap_u32(v->flags);
v->reserved = syms_bswap_u32(v->reserved);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachSegmentCommand32(SYMS_MachSegmentCommand32 *v)
{
syms_bswap_in_place__SYMS_MachLoadCommand(&v->cmd);
v->vmaddr = syms_bswap_u32(v->vmaddr);
v->vmsize = syms_bswap_u32(v->vmsize);
v->fileoff = syms_bswap_u32(v->fileoff);
v->filesize = syms_bswap_u32(v->filesize);
v->maxprot = syms_bswap_u32(v->maxprot);
v->initprot = syms_bswap_u32(v->initprot);
v->nsects = syms_bswap_u32(v->nsects);
v->flags = syms_bswap_u32(v->flags);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachSegmentCommand64(SYMS_MachSegmentCommand64 *v)
{
syms_bswap_in_place__SYMS_MachLoadCommand(&v->cmd);
v->vmaddr = syms_bswap_u64(v->vmaddr);
v->vmsize = syms_bswap_u64(v->vmsize);
v->fileoff = syms_bswap_u64(v->fileoff);
v->filesize = syms_bswap_u64(v->filesize);
v->maxprot = syms_bswap_u32(v->maxprot);
v->initprot = syms_bswap_u32(v->initprot);
v->nsects = syms_bswap_u32(v->nsects);
v->flags = syms_bswap_u32(v->flags);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachSection32(SYMS_MachSection32 *v)
{
v->addr = syms_bswap_u32(v->addr);
v->size = syms_bswap_u32(v->size);
v->offset = syms_bswap_u32(v->offset);
v->align = syms_bswap_u32(v->align);
v->relocoff = syms_bswap_u32(v->relocoff);
v->nreloc = syms_bswap_u32(v->nreloc);
v->flags = syms_bswap_u32(v->flags);
v->reserved1 = syms_bswap_u32(v->reserved1);
v->reserved2 = syms_bswap_u32(v->reserved2);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachSection64(SYMS_MachSection64 *v)
{
v->addr = syms_bswap_u64(v->addr);
v->size = syms_bswap_u64(v->size);
v->offset = syms_bswap_u32(v->offset);
v->align = syms_bswap_u32(v->align);
v->relocoff = syms_bswap_u32(v->relocoff);
v->nreloc = syms_bswap_u32(v->nreloc);
v->flags = syms_bswap_u32(v->flags);
v->reserved1 = syms_bswap_u32(v->reserved1);
v->reserved2 = syms_bswap_u32(v->reserved2);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachSymtabCommand(SYMS_MachSymtabCommand *v)
{
v->cmd = syms_bswap_u32(v->cmd);
v->cmdsize = syms_bswap_u32(v->cmdsize);
v->symoff = syms_bswap_u32(v->symoff);
v->nsyms = syms_bswap_u32(v->nsyms);
v->stroff = syms_bswap_u32(v->stroff);
v->strsize = syms_bswap_u32(v->strsize);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachDySymtabCommand(SYMS_MachDySymtabCommand *v)
{
v->cmd = syms_bswap_u32(v->cmd);
v->cmdsize = syms_bswap_u32(v->cmdsize);
v->ilocalsym = syms_bswap_u32(v->ilocalsym);
v->nlocalsym = syms_bswap_u32(v->nlocalsym);
v->iextdefsym = syms_bswap_u32(v->iextdefsym);
v->nextdefsym = syms_bswap_u32(v->nextdefsym);
v->iundefsym = syms_bswap_u32(v->iundefsym);
v->nundefsym = syms_bswap_u32(v->nundefsym);
v->tocoff = syms_bswap_u32(v->tocoff);
v->ntoc = syms_bswap_u32(v->ntoc);
v->modtaboff = syms_bswap_u32(v->modtaboff);
v->nmodtab = syms_bswap_u32(v->nmodtab);
v->extrefsymoff = syms_bswap_u32(v->extrefsymoff);
v->nextrefsyms = syms_bswap_u32(v->nextrefsyms);
v->indirectsymoff = syms_bswap_u32(v->indirectsymoff);
v->nindirectsyms = syms_bswap_u32(v->nindirectsyms);
v->extreloff = syms_bswap_u32(v->extreloff);
v->nextrel = syms_bswap_u32(v->nextrel);
v->locreloff = syms_bswap_u32(v->locreloff);
v->nlocrel = syms_bswap_u32(v->nlocrel);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachNList32(SYMS_MachNList32 *v)
{
v->n_strx = syms_bswap_u32(v->n_strx);
v->n_desc = syms_bswap_u16(v->n_desc);
v->n_value = syms_bswap_u32(v->n_value);
}

SYMS_API void
syms_bswap_in_place__SYMS_MachNList64(SYMS_MachNList64 *v)
{
v->n_strx = syms_bswap_u32(v->n_strx);
v->n_desc = syms_bswap_u16(v->n_desc);
v->n_value = syms_bswap_u64(v->n_value);
}

#endif
