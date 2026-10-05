################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/extend/Src/dxl_mx64.c \
../Core/extend/Src/uart_communicator.c 

OBJS += \
./Core/extend/Src/dxl_mx64.o \
./Core/extend/Src/uart_communicator.o 

C_DEPS += \
./Core/extend/Src/dxl_mx64.d \
./Core/extend/Src/uart_communicator.d 


# Each subdirectory must supply rules for building sources it contributes
Core/extend/Src/%.o Core/extend/Src/%.su Core/extend/Src/%.cyclo: ../Core/extend/Src/%.c Core/extend/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -DUSE_FULL_LL_DRIVER -DUSE_HAL_DRIVER -DSTM32F446xx -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-extend-2f-Src

clean-Core-2f-extend-2f-Src:
	-$(RM) ./Core/extend/Src/dxl_mx64.cyclo ./Core/extend/Src/dxl_mx64.d ./Core/extend/Src/dxl_mx64.o ./Core/extend/Src/dxl_mx64.su ./Core/extend/Src/uart_communicator.cyclo ./Core/extend/Src/uart_communicator.d ./Core/extend/Src/uart_communicator.o ./Core/extend/Src/uart_communicator.su

.PHONY: clean-Core-2f-extend-2f-Src

