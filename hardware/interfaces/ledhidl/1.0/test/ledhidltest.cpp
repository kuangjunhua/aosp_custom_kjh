//hardware/interfaces/ledhidl/1.0/test/testdemo.cpp

#include <android-base/logging.h>
#include <android/hardware/ledhidl/1.0/ILedService.h>
#include <gtest/gtest.h>
#include <hidl/GtestPrinter.h>
#include <hidl/ServiceManagement.h>

using ::android::hardware::ledhidl::V1_0::ILedService;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

class MyLedServiceTest : public ::testing::TestWithParam<std::string> {
    protected:
        sp<ILedService> service;
        virtual void SetUp() override { //初始化
            service = ILedService::getService(GetParam());
            ASSERT_NE(service, nullptr);
        }
};

TEST_P(MyLedServiceTest, TestMyService_getStatus){
    Return<int32_t> ret = service->getStatus();
    ASSERT_TRUE(ret.isOk());   // 确认调用成功
    int32_t currentStatus = ret; // Return<T> 可以隐式转为 T
    EXPECT_EQ(currentStatus, 0);
}
TEST_P(MyLedServiceTest, TestMyService_operate){
    std::string message;
    int32_t currentStatus;
    service->operate(1, [&](int32_t afterStatus, const auto& msg){
        currentStatus = afterStatus;
        message = msg;
    });
    EXPECT_EQ(currentStatus, 1);
    EXPECT_EQ(message, "status updated");
}

INSTANTIATE_TEST_SUITE_P(
    //测试套件实例名称
    PerInstance,
    //测试夹具类
    MyLedServiceTest,
    //为测试夹具类 MyLedServiceTest 提供参数列表
    //valuesIn(参数列表)，传送给测试夹具类即MyServiceTest以便它初始化.

    //参数生成器,获取IMyService接口的所有实例名称
    testing::ValuesIn(android::hardware::getAllHalInstanceNames(ILedService::descriptor)),
    //参数名称生成器
    android::hardware::PrintInstanceNameToString
);