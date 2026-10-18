// Public Windows SDK declarations, not guessed private interface lengths.
#define CINTERFACE
#include <d3d11_4.h>
#include <stddef.h>
size_t sfnp_fixture_context_slots(unsigned version) {
    switch(version) {
    case 4:return sizeof(ID3D11DeviceContext4Vtbl)/sizeof(void*);
    case 3:return sizeof(ID3D11DeviceContext3Vtbl)/sizeof(void*);
    case 2:return sizeof(ID3D11DeviceContext2Vtbl)/sizeof(void*);
    case 1:return sizeof(ID3D11DeviceContext1Vtbl)/sizeof(void*);
    case 0:return sizeof(ID3D11DeviceContextVtbl)/sizeof(void*);
    default:return 0;
    }
}
