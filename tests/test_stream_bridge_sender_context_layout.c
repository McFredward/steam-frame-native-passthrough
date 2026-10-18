// Public Windows SDK declarations, not guessed private interface lengths.
#define CINTERFACE
#if defined(_MSC_VER)
// Windows SDK public C declarations use intentional nameless structs/unions.
// Limit this allowance to the SDK include; our fixture stays under /W4 /WX.
#pragma warning(push)
#pragma warning(disable:4201)
#endif
#include <d3d11_4.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
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
