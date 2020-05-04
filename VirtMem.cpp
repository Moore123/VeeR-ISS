#include <cmath>
#include "VirtMem.hpp"

using namespace WdRiscv;


VirtMem::VirtMem(Memory& memory, unsigned pageSize)
  : memory_(memory), mode_(Sv32), pageSize_(pageSize)
{
  pageBits_ = static_cast<unsigned>(std::log2(pageSize_));
  unsigned p2PageSize =  unsigned(1) << pageBits_;

  assert(p2PageSize == pageSize);
  assert(pageSize > 1024);
}


bool
VirtMem::translate(size_t va, PrivilegeMode pm, bool read, bool write,
                   bool exec, size_t& pa)
{
  if (mode_ == Bare)
    {
      pa = va;
      return true;
    }

  if (mode_ == Sv32)
    return translate_<Pte32, Va32>(va, pm, read, write, exec, pa);

  if (mode_ == Sv39)
    return translate_<Pte39, Va39>(va, pm, read, write, exec, pa);

  if (mode_ == Sv48)
    return translate_<Pte48, Va48>(va, pm, read, write, exec, pa);

  assert(0 and "Unspupported virtual memory mode.");
  return false;
}    


template<typename PTE, typename VA>
bool
VirtMem::translate_(size_t address, PrivilegeMode privMode,
                    bool read, bool write, bool exec,
                    size_t& pa)
{
  // TBD check xlen against valen.

  PTE pte(0);

  const unsigned levels = pte.levels();
  const unsigned pteSize = pte.size();

  VA va(address);

  uint64_t root = pageTableRoot_;

  // 2.
  int ii = levels - 1;

  while (true)
    {
      // 3.

      uint32_t vpn = va.vpn(ii);
      uint64_t pteAddr = root + vpn*pteSize;

      // TBD: Check pma
      if (! memory_.read(pteAddr, pte.data_))
        return false;  // Access fault.

      // 4.
      if (not pte.valid() or (not pte.read() and pte.write()))
        return false;  // Page fault

      // 5.
      if (not pte.read() and not pte.exec())
        {
          ii = ii - 1;
          if (ii < 0)
            return false;  // Page fault
          root = pte.ppn();
          // goto 3.
        }
      else
        break;  // goto 6.
    }

  // 6.  pte.read_ or pte.exec_ : leaf pte
  if (privMode == PrivilegeMode::User and not pte.user())
    return false;  // Access fault

  if (privMode == PrivilegeMode::Supervisor and pte.user() and
      not supervisorOk_)
    return false;  // Supervisor access to user pages not allowed.
        
  bool pteRead = pte.read() or (execReadable_ and pte.exec());
  if ((read and not pteRead) or (write and not pte.write()) or
      (exec and pte.exec()))
    return false;  // Page fault
  
  // 7.
  if (ii > 0 and pte.ppn1() != 0 and pte.ppn0() != 0)
    return false;  // Page fault

  // 8.
  if (not pte.accessed() or (write and not pte.dirty()))
    {
      // We have a choice:
      // A. Page fault, or
      // B1. Set pte->accessed_ to 1 and, if a write, set pte->dirty_ to 1.
      // B2. Access fault if PMP violation.

      return false;  // Choose A for now
    }

  // 9.
  pa = va.offset();
  if (ii > 0)
    for (int i = ii - 1; i >= 0; --i)
      pa = pa | (va.vpn(i) << pte.paPpnShift(i));

  for (int i = levels - 1; i >= ii; --i)
    pa = pa | pte.ppn(i) << pte.paPpnShift(i);

  return true;
}