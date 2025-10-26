//hardware/interfaces/hellohidl/1.0/test/testdemo.cpp

#include <android-base/logging.h>
#include <android/hardware/hellohidl/1.0/IMyService.h>
#include <gtest/gtest.h>
#include <hidl/GtestPrinter.h>
#include <hidl/ServiceManagement.h>

using ::android::hardware::hellohidl::V1_0::IMyService;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::sp;

class MyServiceTest : public ::testing::TestWithParam<std::string> {
    protected:
        sp<IMyService> service;
        virtual void SetUp() override { //初始化
            service = IMyService::getService(GetParam());
            ASSERT_NE(service, nullptr);
        }
};

TEST_P(MyServiceTest, TestMyService_exampleAdd){
    std::string desp;
    int32_t sum;
    service->exampleAdd(10, 20, [&](int32_t outsum, const auto& outDescript){
        sum = outsum;
        desp = outDescript;
    });
    EXPECT_EQ(sum, 30);
    EXPECT_EQ(desp, "this is the result");
}

INSTANTIATE_TEST_SUITE_P(
    //测试套件实例名称
    PerInstance,
    //测试夹具类
    MyServiceTest,
    //为测试夹具类 MyServiceTest 提供参数列表
    //valuesIn(参数列表)，传送给测试夹具类即MyServiceTest以便它初始化.

    //参数生成器,获取IMyService接口的所有实例名称
    testing::ValuesIn(android::hardware::getAllHalInstanceNames(IMyService::descriptor)),
    //参数名称生成器
    android::hardware::PrintInstanceNameToString
);