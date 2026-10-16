#include "../main/os_profile.c"
#include <assert.h>
#include <stdio.h>
typedef struct { char key[16]; uint8_t value[16]; size_t size; } record_t;
static record_t saved[8], staged[8];
static char opened[32];
static bool commit_fail;
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{ (void)mode; snprintf(opened,sizeof(opened),"%s",name); *h=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *v,size_t *n)
{
    (void)h;
    for(unsigned i=0;i<8;++i) if(!strcmp(saved[i].key,k)) {
        assert(*n>=saved[i].size); *n=saved[i].size; memcpy(v,saved[i].value,*n); return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *v,size_t n)
{
    (void)h; assert(n<=16);
    unsigned i;
    for(i=0;i<8;++i) if(!strcmp(staged[i].key,k)) break;
    if(i==8) for(i=0;i<8;++i) if(!staged[i].key[0]) break;
    assert(i<8); snprintf(staged[i].key,16,"%s",k); staged[i].size=n;
    memcpy(staged[i].value,v,n); return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v)
{ size_t n=1; return nvs_get_blob(h,k,v,&n); }
esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v)
{ return nvs_set_blob(h,k,&v,1); }
esp_err_t nvs_commit(nvs_handle_t h)
{ (void)h; if(commit_fail)return ESP_FAIL; memcpy(saved,staged,sizeof(saved)); return ESP_OK; }
void esp_fill_random(void *v,size_t n) { memset(v,0x35,n); }
static void reboot(void)
{ memcpy(staged,saved,sizeof(staged)); active=0; memset(address,0,6); assert(os_profile_init()==ESP_OK); }
static void check_namespace(const char *expected)
{ nvs_handle_t h; assert(__wrap_nvs_open("nimble_bond",NVS_READWRITE,&h)==ESP_OK); assert(!strcmp(opened,expected)); }
int main(void)
{
    reboot(); assert(os_profile_active()==0);
    uint8_t ubuntu[6],windows[6]; memcpy(ubuntu,os_profile_address(),6);
    assert((ubuntu[5]&0xc0)==0xc0); check_namespace("bond_ubuntu");
    assert(!strcmp(os_profile_peers_namespace(),"peers_ubuntu"));
    assert(os_profile_select(1)==ESP_OK && os_profile_active()==0);
    check_namespace("bond_ubuntu"); // No live namespace change before reboot.
    reboot(); assert(os_profile_active()==1); memcpy(windows,address,6);
    assert(memcmp(ubuntu,windows,6)); check_namespace("bond_windows");
    assert(!strcmp(os_profile_peers_namespace(),"peers_windows"));
    assert(os_profile_select(0)==ESP_OK); reboot(); assert(!memcmp(ubuntu,address,6));
    assert(os_profile_select(1)==ESP_OK); reboot(); assert(!memcmp(windows,address,6));
    commit_fail=true; assert(os_profile_select(0)==ESP_FAIL); commit_fail=false;
    reboot(); assert(os_profile_active()==1 && !memcmp(windows,address,6));
    assert(os_profile_select(2)==ESP_ERR_INVALID_ARG);
    nvs_handle_t h; __wrap_nvs_open("input_switch",NVS_READWRITE,&h);
    assert(!strcmp(opened,"input_switch"));
    puts("OS profiles: persistent identities, isolated namespaces, reboot switch and failed commit passed");
}
